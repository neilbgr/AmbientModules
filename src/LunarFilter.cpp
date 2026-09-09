#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "Widgets.hpp"
#include "dsp/PolivoksFilter.hpp"

// Reimplements the SOLAR 42F's "FILTER" block (manual p.21): two
// independent 12dB Polivoks-style multimode (LP/BP) filters, meant to be
// chained after LunarMixer's stereo L/R output, followed by a shared
// distortion stage (chain: MIX/PAN -> DUAL LP/BP VCF -> DISTORTION, per the
// manual's block diagram).
//
// Confirmed against two Elta Music demo/tutorial videos (not just the
// written manual, which is ambiguous on this point): CV/MOD only ever
// modulate Filter L's frequency, never Filter R's directly and never
// either RES. LINK, when on, makes Filter R's *effective* frequency
// (knob + CV*MOD) follow Filter L's — Filter R's own FREQ knob is then
// bypassed. RES and the LP/BP TYPE switch always stay independent per
// filter, LINK or not:
//   effFreqL = FreqLKnob + CV*MOD                              (always)
//   effFreqR = LINK ? effFreqL : FreqRKnob                      (RES/TYPE never linked)
struct LunarFilter : Module {
    enum ParamIds {
        FREQ_L_PARAM,
        RES_L_PARAM,
        TYPE_L_PARAM,
        FREQ_R_PARAM,
        RES_R_PARAM,
        TYPE_R_PARAM,
        MOD_PARAM,
        DIST_PARAM,
        GAIN_PARAM,
        LINK_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_L_INPUT,
        IN_R_INPUT,
        CV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_L_OUTPUT,
        OUT_R_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        LINK_LIGHT,
        NUM_LIGHTS
    };

    static constexpr float FREQ_MIN_HZ = 20.f;
    static constexpr float FREQ_MAX_HZ = 20000.f;
    static constexpr float FREQ_LOG2_RATIO = 9.965784f; // log2(FREQ_MAX_HZ / FREQ_MIN_HZ)
    // displayBase^knobValue * displayMultiplier reproduces the exact same
    // FREQ_MIN_HZ * 2^(FREQ_LOG2_RATIO * knobValue) mapping used in
    // process() below, since FREQ_MAX_HZ/FREQ_MIN_HZ == 2^FREQ_LOG2_RATIO
    // by construction — the knob's tooltip shows real Hz, not raw 0..1.
    static constexpr float FREQ_DISPLAY_BASE = FREQ_MAX_HZ / FREQ_MIN_HZ;
    static constexpr float MAX_MOD_OCTAVES = 5.f;
    // Drive knob range: unity at GAIN=0 up to 20x pre-clip gain at GAIN=1 —
    // same order of magnitude as other drive-style controls in this pack.
    static constexpr float MAX_DRIVE = 19.f;

    PolivoksFilter filterL, filterR;

    LunarFilter() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(FREQ_L_PARAM, 0.f, 1.f, 0.5f, "Filter L frequency", " Hz", FREQ_DISPLAY_BASE, FREQ_MIN_HZ);
        configParam(RES_L_PARAM, 0.f, 1.f, 0.f, "Filter L resonance", "%", 0.f, 100.f);
        configSwitch(TYPE_L_PARAM, 0.f, 1.f, 0.f, "Filter L type", {"Low-pass", "Band-pass"});
        configParam(FREQ_R_PARAM, 0.f, 1.f, 0.5f, "Filter R frequency", " Hz", FREQ_DISPLAY_BASE, FREQ_MIN_HZ);
        configParam(RES_R_PARAM, 0.f, 1.f, 0.f, "Filter R resonance", "%", 0.f, 100.f);
        configSwitch(TYPE_R_PARAM, 0.f, 1.f, 0.f, "Filter R type", {"Low-pass", "Band-pass"});
        configParam(MOD_PARAM, 0.f, 1.f, 0.f, "CV amount (Filter L only)", "%", 0.f, 100.f);
        configParam(DIST_PARAM, 0.f, 1.f, 0.f, "Distortion blend", "%", 0.f, 100.f);
        configParam(GAIN_PARAM, 0.f, 1.f, 0.f, "Distortion amount", "%", 0.f, 100.f);
        configSwitch(LINK_PARAM, 0.f, 1.f, 0.f, "Link (Filter R follows Filter L frequency)", {"Off", "On"});
        configInput(IN_L_INPUT, "Left");
        configInput(IN_R_INPUT, "Right");
        configInput(CV_INPUT, "Filter L cutoff CV");
        configOutput(OUT_L_OUTPUT, "Left");
        configOutput(OUT_R_OUTPUT, "Right");
    }

    void process(const ProcessArgs& args) override {
        float cv = inputs[CV_INPUT].getVoltage() / 10.f; // +-10V -> +-1, same convention as elsewhere in this pack
        float modOct = params[MOD_PARAM].getValue() * MAX_MOD_OCTAVES * cv;
        float effFreqLOct = FREQ_LOG2_RATIO * params[FREQ_L_PARAM].getValue() + modOct;
        float freqLHz = FREQ_MIN_HZ * std::pow(2.f, effFreqLOct);

        bool link = params[LINK_PARAM].getValue() > 0.5f;
        lights[LINK_LIGHT].setBrightness(link ? 1.f : 0.f);
        float freqRHz;
        if (link) {
            freqRHz = freqLHz;
        } else {
            float effFreqROct = FREQ_LOG2_RATIO * params[FREQ_R_PARAM].getValue();
            freqRHz = FREQ_MIN_HZ * std::pow(2.f, effFreqROct);
        }

        PolivoksFilter::Mode typeL = (params[TYPE_L_PARAM].getValue() > 0.5f) ? PolivoksFilter::MODE_BP : PolivoksFilter::MODE_LP;
        PolivoksFilter::Mode typeR = (params[TYPE_R_PARAM].getValue() > 0.5f) ? PolivoksFilter::MODE_BP : PolivoksFilter::MODE_LP;

        // /5.f: normalize the +-5V Eurorack audio convention to the ~+-1
        // domain PolivoksFilter/fastTanh work in, same convention as
        // LunarVCOCore's dry output before its own x5V output stage.
        float inL = inputs[IN_L_INPUT].getVoltage() / 5.f;
        float inR = inputs[IN_R_INPUT].getVoltage() / 5.f;

        float filteredL = filterL.process(args.sampleRate, inL, freqLHz, params[RES_L_PARAM].getValue(), typeL);
        float filteredR = filterR.process(args.sampleRate, inR, freqRHz, params[RES_R_PARAM].getValue(), typeR);

        float distAmt = params[DIST_PARAM].getValue();
        float driveScale = 1.f + params[GAIN_PARAM].getValue() * MAX_DRIVE;
        float wetL = PolivoksFilter::fastTanh(filteredL * driveScale);
        float wetR = PolivoksFilter::fastTanh(filteredR * driveScale);

        outputs[OUT_L_OUTPUT].setVoltage((filteredL * (1.f - distAmt) + wetL * distAmt) * 5.f);
        outputs[OUT_R_OUTPUT].setVoltage((filteredR * (1.f - distAmt) + wetR * distAmt) * 5.f);
    }
};

struct LunarFilterWidget : ModuleWidget {
    int appliedTheme = -1;

    LunarFilterWidget(LunarFilter* module) {
        setModule(module);
        syncPanelTheme(this, "LunarFilter", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float x1 =  8.485f;
        const float x2 = 14.97f;
        const float x3 = 21.455f;

        const float y1 = 22.99400f;
        const float y2 = 32.63290f;
        const float y3 = 45.98606f;
        const float y4 = 59.69578f;
        const float y5 = 72.f;
        const float y6 = 87.8f;

        addParam(createParamCentered<CKSS>(mm2px(Vec(x1, y1)), module, LunarFilter::TYPE_L_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x1, y2)), module, LunarFilter::RES_L_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x1, y3)), module, LunarFilter::FREQ_L_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x1, y4)), module, LunarFilter::MOD_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1, y5)), module, LunarFilter::CV_INPUT));

        addParam(createParamCentered<CKSS>(mm2px(Vec(x3, y1)), module, LunarFilter::TYPE_R_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x3, y2)), module, LunarFilter::RES_R_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x3, y3)), module, LunarFilter::FREQ_R_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x3, 57.97410f)), module, LunarFilter::LINK_PARAM, LunarFilter::LINK_LIGHT));

        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x1, y6)), module, LunarFilter::DIST_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x3, y6)), module, LunarFilter::GAIN_PARAM));
        
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1, 101.66575f)), module, LunarFilter::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1, 113.16522f)), module, LunarFilter::IN_R_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3, 101.66575f)), module, LunarFilter::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3, 113.16522f)), module, LunarFilter::OUT_R_OUTPUT));
    }

    void step() override {
        syncPanelTheme(this, "LunarFilter", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        appendAmbientThemeMenu(menu);
    }
};

Model* modelLunarFilter = createModel<LunarFilter, LunarFilterWidget>("LunarFilter");
