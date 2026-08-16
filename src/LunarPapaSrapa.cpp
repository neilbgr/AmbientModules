#include "plugin.hpp"
#include "dsp/PapaSrapaCore.hpp"
#include "dsp/AREnvelope.hpp"
#include "PanelTheme.hpp"

// A plain octave-linear (dB-per-mm) knob spends as much rotation on the last
// octave (top half of the range in Hz) as on the first — fine for a wide
// pitch control, but it left too little of the LFO knob's travel usable near
// its upper end. minOct/maxOct/curveExp are set right after configParam<>();
// raw param value is a normalized knob position in [0, 1], reshaped by
// pow(pos, curveExp) before being mapped onto the octave range. curveExp < 1
// front-loads octaves at low knob positions, leaving more travel for the top.
struct RateParamQuantity : ParamQuantity {
    float minOct = 0.f, maxOct = 1.f, curveExp = 1.f;

    float posToOctaves(float pos) const {
        return minOct + (maxOct - minOct) * std::pow(clamp(pos, 0.f, 1.f), curveExp);
    }
    float octavesToPos(float octaves) const {
        float pos = (octaves - minOct) / (maxOct - minOct);
        return std::pow(clamp(pos, 0.f, 1.f), 1.f / curveExp);
    }
    float getDisplayValue() override {
        return std::pow(2.f, posToOctaves(getValue()));
    }
    void setDisplayValue(float hz) override {
        setValue(octavesToPos(std::log2(std::max(hz, 1e-6f))));
    }
};

struct LunarPapaSrapa : Module {
    static constexpr float OUTPUT_VOLTAGE = 5.f; // Eurorack +-5V audio convention
    static constexpr float RATE_MIN_OCT = 0.f;     // 1 Hz: below this, FM/AM cross-mod barely reads as motion
    static constexpr float RATE_MAX_OCT = 8.0314f; // C4 (~261.63 Hz): upper bound
    // NOTE: the core treats RATE_PARAM as octaves relative to 1 Hz (not C4),
    // so 0.f here means 1 Hz, not 1 Hz-relative-to-C4 — hence the explicit
    // log2(261.63) value for the upper bound.
    static constexpr float RATE_CURVE_EXP = 0.6f; // <1: more knob travel near the top of the range
    static constexpr float PITCH_MIN_OCT = -4.f;      // C0 (~16.35 Hz)
    static constexpr float PITCH_MAX_OCT = 10.f / 3.f; // E7 (~2637 Hz), per solar42f_instruct_03_25_v9.pdf p.8

    int theme = 0;

    enum ParamIds {
        RATE_PARAM,         // LFO rate, single knob from 1 Hz up to C4
        PITCH_PARAM,        // audio oscillator pitch, single knob spanning C0..E7
        FM_PARAM,           // switch: modulator -> audio frequency
        AM_PARAM,           // switch: modulator -> audio amplitude
        MOD_PARAM,          // modulation depth
        DIVIDER_PARAM,      // modulator frequency divider
        NOISE_PARAM,        // independent white noise mix
        NOISE_ONLY_PARAM,   // switch: force clean white-noise-only output
        ATTACK_PARAM,
        RELEASE_PARAM,
        HOLD_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        GATE_INPUT,
        PITCH_CV_INPUT,     // 1V/oct, sums onto PITCH_PARAM
        SH_CLOCK_INPUT,     // read from channel 0 only (S&H is monophonic)
        SH_INPUT,           // sample & hold source, normalled to the internal noise; read from channel 0 only (S&H is monophonic)
        ENV_INPUT,
        MOD_CV_INPUT,       // sums onto MOD_PARAM; read from channel 0 only (the internal LFO is monophonic)
        DIVIDER_CV_INPUT,   // sums onto DIVIDER_PARAM; read from channel 0 only (the internal LFO is monophonic)
        NUM_INPUTS
    };
    enum OutputIds {
        LFO_OUTPUT,
        VCO_OUT,
        ENV_OUTPUT,
        SH_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        HOLD_LIGHT,
        ENV_LIGHT,
        NOISE_ONLY_LIGHT,
        FM_LIGHT,
        AM_LIGHT,
        SH_LIGHT,
        NUM_LIGHTS
    };

    // One PapaSrapaCore instance handles 4 poly channels at once (SIMD) —
    // see process() below.
    PapaSrapaCore core[PORT_MAX_CHANNELS / 4];
    // The internal LFO/modulator is always monophonic (see process()) — a
    // single instance shared by every poly channel's FM/AM and by the
    // (mono) LFO_OUTPUT.
    PapaSrapaModulator modulator;
    AREnvelope envelope[PORT_MAX_CHANNELS];
    dsp::SchmittTrigger gateTrigger[PORT_MAX_CHANNELS];
    // Sample & hold is always monophonic too (one clock, one held value),
    // same simplification as the LFO/modulator above. shNoiseState is its
    // own PRNG state, decoupled from each voice's own noise generator (see
    // PapaSrapaCore) — it only ever needs to advance on a clock edge, not
    // continuously every sample.
    dsp::SchmittTrigger shClockTrigger;
    float shValue = 0.f;
    uint32_t shNoiseState = 0x1234567u;
    // The white-noise mix into the audio output is also a single shared
    // source (see process()) rather than one generator per poly voice —
    // decoupled from shNoiseState above since the two are gated separately.
    uint32_t noiseState = 0x1234567u;

    LunarPapaSrapa() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        float rateDefaultPos = std::pow((2.f - RATE_MIN_OCT) / (RATE_MAX_OCT - RATE_MIN_OCT), 1.f / RATE_CURVE_EXP);
        RateParamQuantity* rateQ = configParam<RateParamQuantity>(RATE_PARAM, 0.f, 1.f, rateDefaultPos, "LFO rate", " Hz");
        rateQ->minOct = RATE_MIN_OCT;
        rateQ->maxOct = RATE_MAX_OCT;
        rateQ->curveExp = RATE_CURVE_EXP;
        configParam(PITCH_PARAM, PITCH_MIN_OCT, PITCH_MAX_OCT, 0.f, "Pitch", " Hz", 2.f, dsp::FREQ_C4);
        configInput(PITCH_CV_INPUT, "Pitch CV (1V/oct)");
        configSwitch(FM_PARAM, 0.f, 1.f, 0.f, "FM", {"Off", "On"});
        configSwitch(AM_PARAM, 0.f, 1.f, 0.f, "AM", {"Off", "On"});
        configParam(MOD_PARAM, 0.f, 1.f, 0.5f, "Modulation depth", "%", 0.f, 100.f);
        configInput(MOD_CV_INPUT, "Modulation depth CV");
        configParam(DIVIDER_PARAM, 0.f, 1.f, 0.f, "Divider", "%", 0.f, 100.f);
        configInput(DIVIDER_CV_INPUT, "Divider CV");
        configParam(NOISE_PARAM, 0.f, 1.f, 0.f, "Noise", "%", 0.f, 100.f);
        configSwitch(NOISE_ONLY_PARAM, 0.f, 1.f, 0.f, "Noise only", {"Off", "On"});

        configParam(ATTACK_PARAM, 0.f, 1.f, 0.2f, "Attack", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.2f, "Release", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold (manual gate)", {"Off", "On"});
        configInput(GATE_INPUT, "Gate");

        configInput(SH_CLOCK_INPUT, "Sample & hold clock");
        configInput(SH_INPUT, "Sample & hold input (normalled to internal noise; connect to override)");
        configOutput(SH_OUTPUT, "Sample & hold");

        configOutput(LFO_OUTPUT, "LFO");
        configOutput(VCO_OUT, "VCO (enveloped)");
        configOutput(ENV_OUTPUT, "Envelope");
        configInput(ENV_INPUT, "Envelope CV input (normalled to internal envelope; connect to override)");
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "theme", json_integer(theme));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* themeJ = json_object_get(rootJ, "theme");
        if (themeJ) {
            theme = json_integer_value(themeJ);
        }
    }

    void process(const ProcessArgs& args) override {
        float rateOctaves = RATE_MIN_OCT + (RATE_MAX_OCT - RATE_MIN_OCT) * std::pow(params[RATE_PARAM].getValue(), RATE_CURVE_EXP);

        bool fm = params[FM_PARAM].getValue() > 0.f;
        bool am = params[AM_PARAM].getValue() > 0.f;
        PapaSrapaCore::ModMode mode = fm && am ? PapaSrapaCore::MODE_FM_AM
                                    : fm ? PapaSrapaCore::MODE_FM
                                    : am ? PapaSrapaCore::MODE_AM
                                    : PapaSrapaCore::MODE_OFF;
        lights[FM_LIGHT].setBrightness(fm ? 1.f : 0.f);
        lights[AM_LIGHT].setBrightness(am ? 1.f : 0.f);

        bool noiseOnly = params[NOISE_ONLY_PARAM].getValue() > 0.f;
        lights[NOISE_ONLY_LIGHT].setBrightness(noiseOnly ? 1.f : 0.f);

        bool holdActive = params[HOLD_PARAM].getValue() > 0.f;
        bool envInputConnected = inputs[ENV_INPUT].isConnected();
        bool gateInputConnected = inputs[GATE_INPUT].isConnected();
        // Attack/Release knobs are non-poly (same for every channel) — compute
        // the lambdas once here instead of recomputing approxExp2_taylor5
        // per channel inside the loop below.
        float attackLambda = AREnvelope::lambdaFromKnob(params[ATTACK_PARAM].getValue());
        float releaseLambda = AREnvelope::lambdaFromKnob(params[RELEASE_PARAM].getValue());

        bool lfoOutputConnected = outputs[LFO_OUTPUT].isConnected();
        bool vcoOutputConnected = outputs[VCO_OUT].isConnected();
        bool shInputConnected = inputs[SH_INPUT].isConnected();
        float noiseAmount = params[NOISE_PARAM].getValue();

        // The internal LFO/modulator is always monophonic: MOD_CV_INPUT and
        // DIVIDER_CV_INPUT are read from channel 0 only, so a poly cable
        // patched into either only affects the shared modulator through its
        // first channel, matching the real hardware's single LFO per module.
        float modAmount = clamp(params[MOD_PARAM].getValue() + inputs[MOD_CV_INPUT].getPolyVoltage(0) / 10.f, 0.f, 1.f);
        float dividerAmount = clamp(params[DIVIDER_PARAM].getValue() + inputs[DIVIDER_CV_INPUT].getPolyVoltage(0) / 10.f, 0.f, 1.f);

        // Poly channel count driven by CV, gate, AND the envelope input, so
        // that chaining another module's poly ENV_OUTPUT into ENV_INPUT
        // (overriding the internal envelope/gate entirely) still drives all
        // of its channels even when Pitch CV and Gate are mono or unpatched.
        int channels = std::max(std::max(inputs[PITCH_CV_INPUT].getChannels(), inputs[GATE_INPUT].getChannels()), std::max(inputs[ENV_INPUT].getChannels(), 1));

        // Pass 1: envelopes are cheap (no std::exp) — compute them all up
        // front so we know whether ANY channel needs the expensive
        // oscillator engine before deciding whether to run the mod. Zero-
        // filled so lanes past `channels` in the last partial group of 4
        // (Pass 2 below) read 0 (silent), never uninitialized memory.
        float envValues[PORT_MAX_CHANNELS] = {};
        float envAmounts[PORT_MAX_CHANNELS] = {};
        bool anyComputeAudio = false;
        for (int c = 0; c < channels; c++) {
            float envValue;
            if (envInputConnected) {
                envValue = inputs[ENV_INPUT].getPolyVoltage(c) / 10.f;
            } else {
                gateTrigger[c].process(inputs[GATE_INPUT].getPolyVoltage(c));
                bool gateHigh = holdActive || (gateInputConnected && gateTrigger[c].isHigh());
                envValue = envelope[c].process(args.sampleTime, gateHigh, attackLambda, releaseLambda);
            }
            envValues[c] = envValue;
            float envAmount = clamp(envValue, 0.f, 1.f);
            envAmounts[c] = envAmount;
            if (vcoOutputConnected && envAmount > 0.f)
                anyComputeAudio = true;
        }

        // Mod is only ever used for LFO_OUTPUT or for the audio oscillators'
        // FM/AM (bypassed entirely in Noise-only mode) — skip it otherwise.
        // Computed once regardless of channel count (see PapaSrapaModulator).
        bool needMod = lfoOutputConnected || (anyComputeAudio && !noiseOnly);
        float modSquare = needMod ? modulator.process(args.sampleTime, rateOctaves, dividerAmount) : modulator.lfoOut;

        // The audio noise mix is a single shared source too — one sample per
        // process() call is enough even with the main VCO polyphonic, since
        // every channel mixes in the same value at its own noiseAmount.
        // Computed only when actually needed (mixed in, or noiseOnly bypass).
        bool needNoise = anyComputeAudio && (noiseOnly || noiseAmount > 0.f);
        float noiseSample = needNoise ? PapaSrapaCore::nextNoise(noiseState) : 0.f;

        outputs[LFO_OUTPUT].setChannels(1);
        outputs[LFO_OUTPUT].setVoltage(modulator.lfoOut * OUTPUT_VOLTAGE);

        // Sample & hold is always monophonic: one clock, one held value. The
        // internal noise source only needs to advance exactly on a clock
        // edge — S&H never reads it in between anyway — so there's no
        // "stay fresh every sample" cost at all here (unlike each voice's
        // own noise, which is mixed continuously into the audio below).
        if (shClockTrigger.process(inputs[SH_CLOCK_INPUT].getPolyVoltage(0))) {
            shValue = shInputConnected ? inputs[SH_INPUT].getPolyVoltage(0) / OUTPUT_VOLTAGE
                                        : PapaSrapaCore::nextNoise(shNoiseState);
        }
        outputs[SH_OUTPUT].setChannels(1);
        outputs[SH_OUTPUT].setVoltage(shValue * OUTPUT_VOLTAGE);
        lights[SH_LIGHT].setBrightness(clamp(std::fabs(shValue), 0.f, 1.f));

        float maxEnvValue = 0.f;
        for (int c = 0; c < channels; c++) {
            maxEnvValue = std::max(maxEnvValue, envAmounts[c]);
        }

        // Pass 2: VCO engine, 4 channels (1 SIMD group) at a time.
        for (int base = 0; base < channels; base += 4) {
            simd::float_4 pitchOctaves4 = params[PITCH_PARAM].getValue() + inputs[PITCH_CV_INPUT].getPolyVoltageSimd<simd::float_4>(base);
            simd::float_4 envValue4 = simd::float_4::load(&envValues[base]);
            simd::float_4 envAmount4 = simd::float_4::load(&envAmounts[base]);

            // VCO_OUT is enveloped, so its audio oscillator is only computed
            // when patched AND at least one lane's envelope is above 0
            // (whole group silent otherwise, same idea as Lunar50Drone) —
            // group-level skip instead of the old per-channel one (see
            // PapaSrapaCore.hpp), since a SIMD group can't skip individual
            // lanes cheaply; correctness is unaffected either way since the
            // final output below is scaled by each lane's own envAmount.
            simd::float_4 vco4 = simd::float_4::zero();
            if (vcoOutputConnected && simd::movemask(envAmount4 > 0.f) != 0) {
                vco4 = core[base / 4].process(args.sampleTime, args.sampleRate, pitchOctaves4,
                    modSquare, modAmount, mode, noiseSample, noiseAmount, noiseOnly);
            }

            outputs[VCO_OUT].setVoltageSimd(vco4 * OUTPUT_VOLTAGE * envAmount4, base);
            outputs[ENV_OUTPUT].setVoltageSimd(envValue4 * 10.f, base);
        }

        outputs[VCO_OUT].setChannels(channels);
        outputs[ENV_OUTPUT].setChannels(channels);

        lights[HOLD_LIGHT].setBrightness(holdActive ? 1.f : 0.f);
        lights[ENV_LIGHT].setBrightness(maxEnvValue);
    }
};

struct LunarPapaSrapaWidget : ModuleWidget, ThemedModuleWidget {
    int appliedTheme = -1;

    LunarPapaSrapaWidget(LunarPapaSrapa* module) {
        setModule(module);
        syncPanelTheme(this, "LunarPapaSrapa", module ? module->theme : 0, appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        float W = 52.318f;
        float C = 55.88f / 2.f;
        float W_6 = W / 6.f;
        
        float x1_5 = C - 2.f * W_6, x2_5 = C - W_6, x3_5 = C, x4_5 = C + W_6, x5_5 = C + 2.f * W_6;

        float x1_4 = x1_5, x2_4 = x1_5 + ((x5_5 - x1_5) / 3.f), x3_4 = x1_5 + ((x5_5 - x1_5) * 2.f / 3.f), x4_4 = x5_5;

        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x1_5, 24.f)), module, LunarPapaSrapa::RATE_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2_5, 30.f)), module, LunarPapaSrapa::FM_PARAM, LunarPapaSrapa::FM_LIGHT));
        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x3_5, 24.f)), module, LunarPapaSrapa::MOD_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x4_5, 30.f)), module, LunarPapaSrapa::AM_PARAM, LunarPapaSrapa::AM_LIGHT));
        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x5_5, 24.f)), module, LunarPapaSrapa::DIVIDER_PARAM));        
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x1_5, 38.f)), module, LunarPapaSrapa::LFO_OUTPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3_5, 38.f)), module, LunarPapaSrapa::MOD_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x5_5, 38.f)), module, LunarPapaSrapa::DIVIDER_CV_INPUT));
        
        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x1_5, 55.25f)), module, LunarPapaSrapa::PITCH_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_5, 67.f)), module, LunarPapaSrapa::PITCH_CV_INPUT));

        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x3_5, 55.25f)), module, LunarPapaSrapa::NOISE_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x3_5, 67.f)), module, LunarPapaSrapa::NOISE_ONLY_PARAM, LunarPapaSrapa::NOISE_ONLY_LIGHT));

        addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(50.f, 63.5f)), module, LunarPapaSrapa::SH_LIGHT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_5, 82.5f)), module, LunarPapaSrapa::SH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3_5, 82.5f)), module, LunarPapaSrapa::SH_CLOCK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x5_5, 82.5f)), module, LunarPapaSrapa::SH_OUTPUT));
        
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_4, 98.f)), module, LunarPapaSrapa::ENV_INPUT));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x2_4, 98.f)), module, LunarPapaSrapa::ATTACK_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x3_4, 98.f)), module, LunarPapaSrapa::RELEASE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x4_4, 98.f)), module, LunarPapaSrapa::ENV_OUTPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_4, 113.f)), module, LunarPapaSrapa::GATE_INPUT));
        addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(x2_4 - 5.5f, 113.f - 2.f )), module, LunarPapaSrapa::ENV_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2_4, 113.f)), module, LunarPapaSrapa::HOLD_PARAM, LunarPapaSrapa::HOLD_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x4_4, 113.f)), module, LunarPapaSrapa::VCO_OUT));

    }

    void step() override {
        if (module) syncPanelTheme(this, "LunarPapaSrapa", dynamic_cast<LunarPapaSrapa*>(module)->theme, appliedTheme);
        ModuleWidget::step();
    }

    void applyTheme(int theme, history::ComplexAction* complexAction = nullptr) override {
        LunarPapaSrapa* module = dynamic_cast<LunarPapaSrapa*>(this->module);
        pushIntFieldChange(module, "change theme", module->theme, theme,
            [](engine::Module* m, int v) { dynamic_cast<LunarPapaSrapa*>(m)->theme = v; }, complexAction);
        module->theme = theme;
        appliedTheme = theme;
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("LunarPapaSrapa", theme))));
    }

    void appendContextMenu(Menu* menu) override {
        LunarPapaSrapa* module = dynamic_cast<LunarPapaSrapa*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
            [=]() { return module->theme; },
            [=](int theme) { applyTheme(theme); }
        ));
        appendApplyThemeToAllItem(menu, module->theme);
    }
};

Model* modelLunarPapaSrapa = createModel<LunarPapaSrapa, LunarPapaSrapaWidget>("LunarPapaSrapa");
