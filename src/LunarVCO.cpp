#include "plugin.hpp"
#include "dsp/LunarVCOCore.hpp"
#include "dsp/ADSREnvelope.hpp"
#include "PanelTheme.hpp"

struct LunarVCO : Module {
    enum ParamIds {
        WAVEFORM_PARAM,
        TUNE_PARAM,
        OCTAVE_PARAM,         // switch: 0 = low, 1 = +3 octaves
        SUB_PARAM,            // switch: sub-oscillator on/off
        LINEXP_PARAM,         // switch: 0 = lin, 1 = exp (secondary CV input)
        SHAPE_PARAM,          // pulse width OR morph blend, depending on waveform
        SHAPE_CV_ATTEN_PARAM,
        FM_CV_ATTEN_PARAM,
        ATTACK_PARAM,
        DECAY_PARAM,
        SUSTAIN_PARAM,
        RELEASE_PARAM,
        HOLD_PARAM,
        SELFGEN_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        VOCT_INPUT,
        FM_INPUT,             // secondary "cv" jack (linear or exponential FM)
        SHAPE_CV_INPUT,
        SYNC_INPUT,
        GATE_INPUT,
        ENV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        VCO_OUTPUT,           // enveloped (VCA'd) main output
        ENV_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        OCTAVE_LIGHT,
        SUB_LIGHT,
        LINEXP_LIGHT,
        HOLD_LIGHT,
        ENV_LIGHT,
        SELFGEN_LIGHT,
        NUM_LIGHTS
    };

    // One LunarVCOCore instance handles 4 poly channels at once (SIMD) — see
    // process() below. ADSREnvelope/gate stay scalar per channel: envelope
    // stages can diverge per channel (asynchronous gates), and there's no
    // sin/exp in that hot loop to vectorize (see ADSREnvelope.hpp).
    LunarVCOCore vco[PORT_MAX_CHANNELS / 4];
    ADSREnvelope env[PORT_MAX_CHANNELS];
    dsp::SchmittTrigger gateTrigger[PORT_MAX_CHANNELS];

    LunarVCO() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configSwitch(WAVEFORM_PARAM, 0.f, 5.f, 0.f, "Waveform",
            {"Sine", "Triangle", "Inverted saw", "Square", "Saw to inv. saw (shape = blend)", "Sine to saw (shape = blend)"});
        configParam(TUNE_PARAM, -0.5f, 0.5f, 0.f, "Tune (1 octave range)");
        configSwitch(OCTAVE_PARAM, 0.f, 1.f, 0.f, "Octave", {"Low", "+3"});
        configSwitch(SUB_PARAM, 0.f, 1.f, 0.f, "Sub oscillator", {"Off", "On (-1 octave)"});
        configSwitch(LINEXP_PARAM, 0.f, 1.f, 0.f, "FM mode", {"Linear", "Exponential"});
        configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape (pulse width / morph blend)");
        configParam(SHAPE_CV_ATTEN_PARAM, -1.f, 1.f, 0.f, "Shape CV amount", "%", 0.f, 100.f);
        configParam(FM_CV_ATTEN_PARAM, -1.f, 1.f, 0.f, "FM amount", "%", 0.f, 100.f);
        configInput(VOCT_INPUT, "V/oct");
        configInput(FM_INPUT, "FM");
        configInput(SHAPE_CV_INPUT, "Shape CV");
        configInput(SYNC_INPUT, "Sync");
        configOutput(VCO_OUTPUT, "Audio Out");

        configParam(ATTACK_PARAM, 0.f, 1.f, 0.2f, "Attack", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.2f, "Decay", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.7f, "Sustain", "%", 0.f, 100.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.2f, "Release", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold", {"Off", "On"});
        configSwitch(SELFGEN_PARAM, 0.f, 1.f, 0.f, "Self-generation", {"Off", "On"});
        configInput(GATE_INPUT, "Gate");
        configOutput(ENV_OUTPUT, "Envelope Out");
        configInput(ENV_INPUT, "Envelope In (overrides internal envelope, gate and hold when connected)");
    }

    void process(const ProcessArgs& args) override {
        // Poly channel count driven by V/oct, gate, the envelope input, AND
        // FM/Shape CV/Sync — the latter three are already read per-channel
        // below (getPolyVoltageSimd) but didn't used to drive the channel
        // count themselves, so a poly cable patched only into one of them
        // (V/oct and Gate left mono) used to be silently truncated to 1
        // channel. Also lets chaining another module's poly ENV_OUTPUT into
        // ENV_INPUT (overriding the internal envelope/gate entirely) still
        // drive all of its channels even when every other input is mono or
        // unpatched.
        int channels = std::max({inputs[VOCT_INPUT].getChannels(), inputs[GATE_INPUT].getChannels(),
                                  inputs[FM_INPUT].getChannels(), inputs[SHAPE_CV_INPUT].getChannels(),
                                  inputs[SYNC_INPUT].getChannels(), inputs[ENV_INPUT].getChannels(), 1});

        bool expMode = params[LINEXP_PARAM].getValue() > 0.f;
        bool octaveOn = params[OCTAVE_PARAM].getValue() > 0.f;
        bool subOscOn = params[SUB_PARAM].getValue() > 0.f;
        bool hold = params[HOLD_PARAM].getValue() > 0.f;
        bool selfGen = params[SELFGEN_PARAM].getValue() > 0.f;
        int waveform = (int)params[WAVEFORM_PARAM].getValue();
        float tune = params[TUNE_PARAM].getValue();
        float shapeParam = params[SHAPE_PARAM].getValue();
        float shapeCvAtten = params[SHAPE_CV_ATTEN_PARAM].getValue();
        float fmCvAtten = params[FM_CV_ATTEN_PARAM].getValue();
        float sustain = params[SUSTAIN_PARAM].getValue();
        // Attack/Decay/Release knobs are non-poly (same for every channel) —
        // compute the lambdas once here instead of recomputing
        // approxExp2_taylor5 per channel inside the loop below.
        float attackLambda = ADSREnvelope::lambdaFromKnob(params[ATTACK_PARAM].getValue());
        float decayLambda = ADSREnvelope::lambdaFromKnob(params[DECAY_PARAM].getValue());
        float releaseLambda = ADSREnvelope::lambdaFromKnob(params[RELEASE_PARAM].getValue());
        bool envInputConnected = inputs[ENV_INPUT].isConnected();
        bool gateInputConnected = inputs[GATE_INPUT].isConnected();
        bool vcoOutputConnected = outputs[VCO_OUTPUT].isConnected();

        // Pass 1: envelopes are cheap scalar per-channel state (ADSREnvelope
        // isn't vectorized — its stages can diverge per channel on
        // asynchronous gates, see ADSREnvelope.hpp) — compute them all up
        // front so Pass 2 can batch the VCO engine in groups of 4 channels
        // and know per-group whether any lane actually needs it. Zero-filled
        // so lanes past `channels` in the last partial group of 4 read 0
        // (silent), never uninitialized memory.
        float envValues[PORT_MAX_CHANNELS] = {};
        float maxEnvValue = 0.f;
        for (int c = 0; c < channels; c++) {
            float envValue;
            if (envInputConnected) {
                envValue = clamp(inputs[ENV_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);
            } else {
                gateTrigger[c].process(inputs[GATE_INPUT].getPolyVoltage(c));
                bool gate = gateTrigger[c].isHigh();
                // Priority: ENV input > Hold button > Gate input.
                bool effectiveGate = hold || (gateInputConnected && gate);
                envValue = env[c].process(args.sampleTime, effectiveGate, selfGen, attackLambda, decayLambda, sustain, releaseLambda);
            }
            envValues[c] = envValue;
            maxEnvValue = std::max(maxEnvValue, envValue);
        }

        // Pass 2: VCO engine, 4 channels (1 SIMD group) at a time.
        for (int base = 0; base < channels; base += 4) {
            simd::float_4 envValue4 = simd::float_4::load(&envValues[base]);

            // Group silent (all 4 lanes' envelope at 0, reliable once
            // STAGE_IDLE/SUSTAIN settle — see ADSREnvelope) or VCO_OUTPUT not
            // patched: skip the AS3340-style oscillator engine for the whole
            // group of 4 (same idea as the old per-channel skip, just at
            // group granularity — see plan notes on this tradeoff).
            simd::float_4 dry4 = simd::float_4::zero();
            if (vcoOutputConnected && simd::movemask(envValue4 > 0.f) != 0) {
                simd::float_4 pitch4 = tune + inputs[VOCT_INPUT].getPolyVoltageSimd<simd::float_4>(base);
                simd::float_4 fmCv4 = inputs[FM_INPUT].getPolyVoltageSimd<simd::float_4>(base) * fmCvAtten;
                simd::float_4 expFm4 = expMode ? fmCv4 : simd::float_4::zero();
                simd::float_4 linFm4 = expMode ? simd::float_4::zero() : fmCv4 * 100.f; // Hz scale, adjustable by ear

                simd::float_4 shape4 = simd::clamp(shapeParam
                    + inputs[SHAPE_CV_INPUT].getPolyVoltageSimd<simd::float_4>(base) / 10.f * shapeCvAtten, 0.f, 1.f);

                dry4 = vco[base / 4].process(args.sampleTime, args.sampleRate, pitch4, expFm4, linFm4,
                    waveform, shape4, octaveOn, subOscOn, inputs[SYNC_INPUT].getPolyVoltageSimd<simd::float_4>(base));
            }

            outputs[VCO_OUTPUT].setVoltageSimd(dry4 * 5.f * envValue4, base);
            outputs[ENV_OUTPUT].setVoltageSimd(envValue4 * 10.f, base);
        }
        outputs[VCO_OUTPUT].setChannels(channels);
        outputs[ENV_OUTPUT].setChannels(channels);

        lights[OCTAVE_LIGHT].setBrightness(octaveOn ? 1.f : 0.f);
        lights[SUB_LIGHT].setBrightness(subOscOn ? 1.f : 0.f);
        lights[LINEXP_LIGHT].setBrightness(expMode ? 1.f : 0.f);
        lights[HOLD_LIGHT].setBrightness(hold ? 1.f : 0.f);
        lights[ENV_LIGHT].setBrightness(maxEnvValue);
        lights[SELFGEN_LIGHT].setBrightness(selfGen ? 1.f : 0.f);
    }
};

struct LunarVCOWidget : ModuleWidget {
    int appliedTheme = -1;

    LunarVCOWidget(LunarVCO* module) {
        setModule(module);
        syncPanelTheme(this, "LunarVCO", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        
        const float x1div3 = 11.37f;
        const float x2div3 = 22.84f;
        const float x3div3 = 34.39f;

        const float x1div4 = 7.3f;
        const float x2div4 = 17.57f;
        const float x3div4 = 28.f;
        const float x4div4 = 38.29f;

        const float pnl1_y1 = 24.f;
        const float pnl1_y2 = 34.5f;
        const float pnl1_y3 = 43.f;

        const float pnl2_y1 = 62.f;
        const float pnl2_y2 = 73.f;

        const float pnl3_y1 = 90.f;
        const float pnl3_y2 = 100.f;
        const float pnl3_y3 = 113.f;

        addParam(createParamCentered<Rogan2PSGreen>(mm2px(Vec(14.5f, 27.5f)), module, LunarVCO::WAVEFORM_PARAM));
        
        addParam(createParamCentered<Rogan1PGreen>(mm2px(Vec(x3div3, pnl1_y1)), module, LunarVCO::SHAPE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(x3div3, pnl1_y2)), module, LunarVCO::SHAPE_CV_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl1_y3)), module, LunarVCO::SHAPE_CV_INPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl1_y3)), module, LunarVCO::SYNC_INPUT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl1_y3)), module, LunarVCO::SUB_PARAM, LunarVCO::SUB_LIGHT));

        addParam(createParamCentered<Rogan1PGreen>(mm2px(Vec(x1div3, pnl2_y1)), module, LunarVCO::TUNE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl2_y2)), module, LunarVCO::VOCT_INPUT));  

        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl2_y1)), module, LunarVCO::OCTAVE_PARAM, LunarVCO::OCTAVE_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl2_y2)), module, LunarVCO::LINEXP_PARAM, LunarVCO::LINEXP_LIGHT));

        addParam(createParamCentered<Rogan1PGreen>(mm2px(Vec(x3div3, pnl2_y1)), module, LunarVCO::FM_CV_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl2_y2)), module, LunarVCO::FM_INPUT));
        
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x1div4, pnl3_y1)), module, LunarVCO::ATTACK_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x2div4, pnl3_y1)), module, LunarVCO::DECAY_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x3div4, pnl3_y1)), module, LunarVCO::SUSTAIN_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x4div4, pnl3_y1)), module, LunarVCO::RELEASE_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl3_y2)), module, LunarVCO::ENV_INPUT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl3_y2)), module, LunarVCO::SELFGEN_PARAM, LunarVCO::SELFGEN_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl3_y2)), module, LunarVCO::ENV_OUTPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl3_y3)), module, LunarVCO::GATE_INPUT));
        addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(x2div3 - 5.5f, pnl3_y3 - 2.f)), module, LunarVCO::ENV_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl3_y3)), module, LunarVCO::HOLD_PARAM, LunarVCO::HOLD_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl3_y3)), module, LunarVCO::VCO_OUTPUT));
    }

    void step() override {
        syncPanelTheme(this, "LunarVCO", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        appendAmbientThemeMenu(menu);
    }
};

Model* modelLunarVCO = createModel<LunarVCO, LunarVCOWidget>("LunarVCO");
