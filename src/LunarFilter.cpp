#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "Widgets.hpp"
#include "FilterExpanderMessage.hpp"
#include "dsp/PolivoksFilter.hpp"

// Reimplements the SOLAR 42F's "FILTER" block (manual p.21): two
// independent 12dB Polivoks-style multimode (LP/BP) filters, meant to be
// chained after LunarMixer's stereo L/R output, followed by a shared
// distortion stage (chain: MIX/PAN -> DUAL LP/BP VCF -> DISTORTION, per the
// manual's block diagram).
//
// Confirmed against two Elta Music demo/tutorial videos (not just the
// written manual, which is ambiguous on this point): on the real hardware,
// CV/MOD only ever modulate Filter L's frequency, never Filter R's
// directly and never either RES. This module adds a second CV/MOD pair
// (CV_R/MOD_R) past the real hardware so Filter R's frequency can also be
// modulated independently. LINK, when on, makes Filter R's *effective*
// frequency follow Filter L's instead — Filter R's own FREQ knob (and its
// own CV_R/MOD_R contribution, which then adds onto Filter L's instead of
// driving Filter R directly) is bypassed. RES and the LP/BP TYPE switch
// always stay independent per filter, LINK or not:
//   effFreqL = FreqLKnob + CV_L*MOD_L + (LINK ? CV_R*MOD_R : 0)  (always)
//   effFreqR = LINK ? effFreqL : FreqRKnob + CV_R*MOD_R           (RES/TYPE never linked)
struct LunarFilter : Module {
    enum ParamIds {
        FREQ_L_PARAM,
        RES_L_PARAM,
        TYPE_L_PARAM,
        FREQ_R_PARAM,
        RES_R_PARAM,
        TYPE_R_PARAM,
        MOD_L_PARAM,
        MOD_R_PARAM,
        DIST_PARAM,
        DIST_MOD_PARAM,
        GAIN_PARAM,
        LINK_PARAM,
        BLEND_PARAM,
        MASTER_PARAM,
        BLEND_ATTEN_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_L_INPUT,
        IN_R_INPUT,
        CV_L_INPUT,
        CV_R_INPUT,
        DIST_CV_INPUT,
        RETURN_L_INPUT,
        RETURN_R_INPUT,
        BLEND_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_L_OUTPUT,
        OUT_R_OUTPUT,
        SEND_L_OUTPUT,
        SEND_R_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        LINK_LIGHT,
        NUM_LIGHTS
    };
    // Send/Return I/O layout: two separate mono jacks per side, or a single
    // "Left" jack per direction carrying a 2-channel poly cable (ch0 = L,
    // ch1 = R) — lets an external genuinely-polyphonic module (e.g.
    // Fundamental's VCF, one poly cable, no L/R jacks) sit in the insert
    // loop without an extra pair of mono-to-poly utility modules.
    enum IOMode { IO_MONO_PAIR = 0, IO_POLY_BUS = 1 };
    int ioMode = IO_MONO_PAIR;

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
        // True bipolar attenuverters, same range/display convention as
        // BLEND_ATTEN_PARAM below (was 0..1 unipolar-only).
        configParam(MOD_L_PARAM, -1.f, 1.f, 0.f, "CV L amount", "%", 0.f, 100.f);
        configParam(MOD_R_PARAM, -1.f, 1.f, 0.f, "CV R amount", "%", 0.f, 100.f);
        configParam(DIST_PARAM, 0.f, 1.f, 0.f, "Distortion blend", "%", 0.f, 100.f);
        // True bipolar attenuverter, same convention as BLEND_ATTEN_PARAM
        // below — default 0 so an unpatched CV input has no effect.
        configParam(DIST_MOD_PARAM, -1.f, 1.f, 0.f, "Distortion blend CV amount", "%", 0.f, 100.f);
        configParam(GAIN_PARAM, 0.f, 1.f, 0.f, "Distortion amount", "%", 0.f, 100.f);
        configSwitch(LINK_PARAM, 0.f, 1.f, 0.f, "Link (Filter R follows Filter L frequency)", {"Off", "On"});
        // Default 0 (fully dry) so a patch saved before this feature existed
        // sounds identical: no Return patched, no Blend, insert loop mixes
        // in nothing regardless of ioMode.
        configParam(BLEND_PARAM, 0.f, 1.f, 0.f, "Insert loop blend (dry/return)", "%", 0.f, 100.f);
        // Default 1 (unity), same convention as LunarMixer::MASTER_VOL_PARAM.
        configParam(MASTER_PARAM, 0.f, 1.f, 1.f, "Master level", "%", 0.f, 100.f);
        // True bipolar attenuverter, same convention as Lunar50Drone::ATTEN_PARAM/
        // LunarVCO's *_CV_ATTEN_PARAM — default 0 so an unpatched CV input has
        // no effect regardless of the CV's own voltage.
        configParam(BLEND_ATTEN_PARAM, -1.f, 1.f, 0.f, "Insert blend CV amount", "%", 0.f, 100.f);
        configInput(IN_L_INPUT, "Left");
        configInput(IN_R_INPUT, "Right");
        configInput(CV_L_INPUT, "Filter L cutoff CV");
        configInput(CV_R_INPUT, "Filter R cutoff CV (sums onto Filter L instead when Link is on)");
        configInput(DIST_CV_INPUT, "Distortion blend CV");
        configInput(RETURN_L_INPUT, "Insert return L (poly: 2ch L+R bus when Send/Return I/O is set to 1 poly jack)");
        configInput(RETURN_R_INPUT, "Insert return R");
        configInput(BLEND_CV_INPUT, "Insert blend CV");
        configOutput(OUT_L_OUTPUT, "Left");
        configOutput(OUT_R_OUTPUT, "Right");
        configOutput(SEND_L_OUTPUT, "Insert send L (poly: 2ch L+R bus when Send/Return I/O is set to 1 poly jack)");
        configOutput(SEND_R_OUTPUT, "Insert send R");

        leftExpander.producerMessage = new FilterExpanderMessage;
        leftExpander.consumerMessage = new FilterExpanderMessage;
    }

    ~LunarFilter() override {
        delete static_cast<FilterExpanderMessage*>(leftExpander.producerMessage);
        delete static_cast<FilterExpanderMessage*>(leftExpander.consumerMessage);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "ioMode", json_integer(ioMode));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* ioModeJ = json_object_get(rootJ, "ioMode")) {
            ioMode = json_integer_value(ioModeJ);
        }
    }

    void process(const ProcessArgs& args) override {
        // +-10V -> +-1, same convention as elsewhere in this pack.
        float cvL = inputs[CV_L_INPUT].getVoltage() / 10.f;
        float cvR = inputs[CV_R_INPUT].getVoltage() / 10.f;
        float modOctFromL = params[MOD_L_PARAM].getValue() * MAX_MOD_OCTAVES * cvL;
        float modOctFromR = params[MOD_R_PARAM].getValue() * MAX_MOD_OCTAVES * cvR;

        bool link = params[LINK_PARAM].getValue() > 0.5f;
        lights[LINK_LIGHT].setBrightness(link ? 1.f : 0.f);

        // Link off: CV_L/MOD_L drives Filter L, CV_R/MOD_R drives Filter R,
        // fully independently. Link on: both sum onto Filter L's effective
        // frequency instead, which Filter R then follows (below).
        float modOctL = modOctFromL + (link ? modOctFromR : 0.f);
        float effFreqLOct = FREQ_LOG2_RATIO * params[FREQ_L_PARAM].getValue() + modOctL;
        float freqLHz = FREQ_MIN_HZ * std::pow(2.f, effFreqLOct);

        float freqRHz;
        if (link) {
            freqRHz = freqLHz;
        } else {
            float effFreqROct = FREQ_LOG2_RATIO * params[FREQ_R_PARAM].getValue() + modOctFromR;
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

        // +-10V -> +-1, same convention as CV_L/CV_R/BLEND_CV above.
        float distCv = inputs[DIST_CV_INPUT].getVoltage() / 10.f;
        float distAmt = clamp(params[DIST_PARAM].getValue() + distCv * params[DIST_MOD_PARAM].getValue(), 0.f, 1.f);
        float driveScale = 1.f + params[GAIN_PARAM].getValue() * MAX_DRIVE;
        float wetL = PolivoksFilter::fastTanh(filteredL * driveScale);
        float wetR = PolivoksFilter::fastTanh(filteredR * driveScale);

        // Today's filtered+distorted signal becomes the pre-insert ("dry")
        // signal: what's sent to the external FX loop, and what a bypassed
        // or unpatched side falls back to.
        float preInsertL = (filteredL * (1.f - distAmt) + wetL * distAmt) * 5.f;
        float preInsertR = (filteredR * (1.f - distAmt) + wetR * distAmt) * 5.f;

        bool polyBus = (ioMode == IO_POLY_BUS);

        if (polyBus) {
            outputs[SEND_L_OUTPUT].setChannels(2);
            outputs[SEND_L_OUTPUT].setVoltage(preInsertL, 0);
            outputs[SEND_L_OUTPUT].setVoltage(preInsertR, 1);
        } else {
            outputs[SEND_L_OUTPUT].setChannels(1);
            outputs[SEND_L_OUTPUT].setVoltage(preInsertL);
            outputs[SEND_R_OUTPUT].setChannels(1);
            outputs[SEND_R_OUTPUT].setVoltage(preInsertR);
        }

        // Return: normalled to the dry signal when nothing is patched, so
        // the insert loop is transparent until something's actually
        // returned.
        float retL = preInsertL;
        float retR = preInsertR;
        if (polyBus) {
            if (inputs[RETURN_L_INPUT].isConnected()) {
                retL = inputs[RETURN_L_INPUT].getVoltage(0);
                // isPolyphonic() (channels > 1), not getPolyVoltage(1): that
                // falls back to channel 0 for a monophonic cable, which
                // would silently duplicate L into R if a mono cable is
                // patched into what's configured as the poly bus jack.
                retR = inputs[RETURN_L_INPUT].isPolyphonic() ? inputs[RETURN_L_INPUT].getVoltage(1) : preInsertR;
            }
        } else {
            if (inputs[RETURN_L_INPUT].isConnected()) {
                retL = inputs[RETURN_L_INPUT].getVoltage();
            }
            if (inputs[RETURN_R_INPUT].isConnected()) {
                retR = inputs[RETURN_R_INPUT].getVoltage();
            }
        }

        // Single shared Blend knob (dry/return) plus CV with a bipolar
        // attenuverter, same +-10V -> +-1 convention as CV_L/CV_R above, same
        // manual linear crossfade style as the distortion blend above. Blend
        // at 0 already reproduces the old per-side bypass switches' effect
        // (pure dry), so those were dropped as redundant.
        float blendCv = inputs[BLEND_CV_INPUT].getVoltage() / 10.f;
        float blend = clamp(params[BLEND_PARAM].getValue() + blendCv * params[BLEND_ATTEN_PARAM].getValue(), 0.f, 1.f);
        float blendedL = preInsertL * (1.f - blend) + retL * blend;
        float blendedR = preInsertR * (1.f - blend) + retR * blend;

        // Single Master knob at the very end of the chain, same convention
        // as LunarMixer::MASTER_VOL_PARAM (plain multiply, no dB curve).
        float masterVol = params[MASTER_PARAM].getValue();
        float outVoltL = blendedL * masterVol;
        float outVoltR = blendedR * masterVol;
        outputs[OUT_L_OUTPUT].setVoltage(outVoltL);
        outputs[OUT_R_OUTPUT].setVoltage(outVoltR);

        // Written unconditionally into our own leftExpander buffer; it's up
        // to whoever reads it (an adjacent LunarMixer) to check our model
        // before interpreting it — see FilterExpanderMessage.hpp. Reflects
        // the fully processed (post-insert, post-Master) output.
        auto* msg = static_cast<FilterExpanderMessage*>(leftExpander.producerMessage);
        msg->outL = outVoltL;
        msg->outR = outVoltR;
        leftExpander.requestMessageFlip();
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

        const float x0 = 2.f, dx = 6.485f;
        const float y0 = 16.509121f, dy = 6.66023f;

        const float x1 = x0 + dx;
        const float x2 = x1 + dx;
        const float x3 = x2 + dx;
        const float x4 = x3 + dx;
        const float x5 = x4 + dx;
        const float x6 = x5 + dx;
        const float x7 = x6 + dx;

        const float y1 = y0 + dy;
        const float y2 = y1 + dy;
        const float y3 = y2 + dy;
        const float y4 = y3 + dy;
        const float y5 = y4 + dy;
        const float y6 = y5 + dy;
        const float y7 = y6 + dy;
        const float y8 = y7 + dy;
        const float y9 = y8 + dy;
        const float y10 = y9 + dy;
        // const float y11 = y10 + dy; // unused
        // const float y12 = y11 + dy; // unused

        addParam(createParamCentered<CKSS>(mm2px(Vec(x1, y1)), module, LunarFilter::TYPE_L_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(x3, y1)), module, LunarFilter::TYPE_R_PARAM));
        
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x1, y3)), module, LunarFilter::RES_L_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x3, y3)), module, LunarFilter::RES_R_PARAM));

        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x1, y5)), module, LunarFilter::FREQ_L_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x3, y5)), module, LunarFilter::FREQ_R_PARAM));

        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2, y7 - dy/2.f)), module, LunarFilter::LINK_PARAM, LunarFilter::LINK_LIGHT));

        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x1, y8)), module, LunarFilter::MOD_L_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x3, y8)), module, LunarFilter::MOD_R_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1, y10)), module, LunarFilter::CV_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3, y10)), module, LunarFilter::CV_R_INPUT));

        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x5, y1)), module, LunarFilter::GAIN_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x5, y3)), module, LunarFilter::DIST_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x5, y5)), module, LunarFilter::DIST_MOD_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x5, y7)), module, LunarFilter::DIST_CV_INPUT));

        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x7, y1)), module, LunarFilter::BLEND_PARAM));
        addParam(createParamCentered<RoganMedSmallOrange>(mm2px(Vec(x7, y3)), module, LunarFilter::BLEND_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x7, y5)), module, LunarFilter::BLEND_CV_INPUT));

        addParam(createParamCentered<RoganOrange>(mm2px(Vec(x6, y10 - dy/2.f)), module, LunarFilter::MASTER_PARAM));
 
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1, 101.66575f)), module, LunarFilter::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1, 113.16522f)), module, LunarFilter::IN_R_INPUT));
        
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3, 101.66575f)), module, LunarFilter::SEND_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3, 113.16522f)), module, LunarFilter::SEND_R_OUTPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x5, 101.66575f)), module, LunarFilter::RETURN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x5, 113.16522f)), module, LunarFilter::RETURN_R_INPUT));


        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x7, 101.66575f)), module, LunarFilter::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x7, 113.16522f)), module, LunarFilter::OUT_R_OUTPUT));

        updateInsertPortVisibility();
    }

    // Hides the R-side Send/Return jacks when the module is set to the
    // single-poly-jack bus mode (module == nullptr in the module-browser
    // preview context, same guard other AmbientModules widgets use — falls
    // back to the mono-pair layout there).
    void updateInsertPortVisibility() {
        LunarFilter* m = dynamic_cast<LunarFilter*>(this->module);
        bool monoPair = (m == nullptr) || (m->ioMode == LunarFilter::IO_MONO_PAIR);
        getOutput(LunarFilter::SEND_R_OUTPUT)->visible = monoPair;
        getInput(LunarFilter::RETURN_R_INPUT)->visible = monoPair;
    }

    void step() override {
        syncPanelTheme(this, "LunarFilter", appliedTheme);
        updateInsertPortVisibility();
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        appendAmbientThemeMenu(menu);

        LunarFilter* module = dynamic_cast<LunarFilter*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Send/Return I/O",
            {"2 mono jacks (L/R)", "1 poly jack (2ch bus on Left)"},
            [=]() { return module->ioMode; },
            [=](int index) {
                pushIntFieldChange(module, "change send/return I/O mode", module->ioMode, index,
                    [](engine::Module* m, int v) { dynamic_cast<LunarFilter*>(m)->ioMode = v; });
                module->ioMode = index;
                updateInsertPortVisibility();
                // Only the R-side jacks ever become invisible/unreachable
                // (the L-side jacks stay visible in both modes, only their
                // channel count changes) — so they're the only ones that can
                // end up with a "zombie" cable driving/reading a hidden port.
                APP->scene->rack->clearCablesOnPort(getOutput(LunarFilter::SEND_R_OUTPUT));
                APP->scene->rack->clearCablesOnPort(getInput(LunarFilter::RETURN_R_INPUT));
            }
        ));
    }
};

Model* modelLunarFilter = createModel<LunarFilter, LunarFilterWidget>("LunarFilter");
