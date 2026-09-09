#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "Widgets.hpp"
#include "dsp/EnvelopeFollower.hpp"

// Reimplements the SOLAR 42F's "CONTACT MIC + ENVELOPE FOLLOWER" block
// (manual p.12): a preamp, an envelope follower (Attack/Release -> CV out),
// and a gate detector (+8V out). The real hardware normally reads its
// internal piezo mic unless an external source is patched into the
// EXT.SOURCE jack, which then takes over instead; there is no internal
// piezo to simulate in software, so this module only exposes that
// external-source path — a single audio input.
// No status LEDs (envelope level / gate activity / clipping on the real
// panel): a patched cable already shows the signal is live, a light would
// be redundant here.
struct LunarDetector : Module {
    enum ParamIds {
        GAIN_PARAM,
        ATTACK_PARAM,
        RELEASE_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ENV_OUTPUT,
        GATE_OUTPUT,
        NUM_OUTPUTS
    };

    EnvelopeFollower follower;

    LunarDetector() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
        // Unlike the real hardware's preamp (up to 40dB, needed for
        // genuine impedance-matching/tiny-signal gain on a physical piezo
        // or line input), this module's In jack already carries
        // Eurorack-normalized audio — no comparable boost is needed, only
        // occasional trim. Range: 0 (-inf dB, full cut) to 2.0 (+6dB
        // headroom), unity/0dB by default — same displayBase/-Multiplier
        // convention Fundamental's VCMixer uses for its own "-inf..+6dB"
        // level knobs (configParam(..., 0.0, 2.0, 1.0, "...", " dB", -10, 20)).
        configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Gain", " dB", -10.f, 20.f);
        // displayBase = MAX_TIME/MIN_TIME in ms (15000), displayMultiplier
        // = MIN_TIME in ms (1) — reproduces EnvelopeFollower::lambdaFromKnob's
        // exact 1ms..15s exponential taper as a real millisecond readout.
        configParam(ATTACK_PARAM, 0.f, 1.f, 0.f, "Attack", " ms", 15000.f, 1.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.f, "Release", " ms", 15000.f, 1.f);
        configInput(IN_INPUT, "External source");
        configOutput(ENV_OUTPUT, "Envelope");
        configOutput(GATE_OUTPUT, "Gate");
    }

    void process(const ProcessArgs& args) override {
        float gain = params[GAIN_PARAM].getValue(); // already linear (0 = -inf dB, 1 = unity, 2 = +6dB)
        // /10.f: normalize the +-10V Eurorack convention down to the [0,1]
        // range EnvelopeFollower/AREnvelope both work in.
        float signal = inputs[IN_INPUT].getVoltage() * gain / 10.f;

        float attackLambda = EnvelopeFollower::lambdaFromKnob(params[ATTACK_PARAM].getValue());
        float releaseLambda = EnvelopeFollower::lambdaFromKnob(params[RELEASE_PARAM].getValue());
        float env = follower.process(args.sampleTime, signal, attackLambda, releaseLambda);
        bool gate = follower.processGate(env);

        outputs[ENV_OUTPUT].setVoltage(env * 10.f);
        outputs[GATE_OUTPUT].setVoltage(gate ? 8.f : 0.f);
    }
};

struct LunarDetectorWidget : ModuleWidget {
    int appliedTheme = -1;

    LunarDetectorWidget(LunarDetector* module) {
        setModule(module);
        syncPanelTheme(this, "LunarDetector", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Placeholder layout: single column, top to bottom: IN, GAIN,
        // ATTACK, RELEASE, ENV, GATE. Panel WIP in Inkscape.
        const float centerX = 15.f;
        const float topMargin = 15.f;
        const float rowPitch = 17.f;

        float y = topMargin;
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(centerX, y)), module, LunarDetector::IN_INPUT));
        y += rowPitch;
        // Same knob as LunarSequencer's Rate/Step CV knobs (Rogan1PRed,
        // stock Rack SDK component, ~10.6mm), per Neil's ask to match it
        // exactly in size and color rather than this pack's own custom
        // Valley-derived RoganMedSmall family.
        addParam(createParamCentered<Rogan1PRed>(mm2px(Vec(centerX, y)), module, LunarDetector::GAIN_PARAM));
        y += rowPitch;
        addParam(createParamCentered<Rogan1PRed>(mm2px(Vec(centerX, y)), module, LunarDetector::ATTACK_PARAM));
        y += rowPitch;
        addParam(createParamCentered<Rogan1PRed>(mm2px(Vec(centerX, y)), module, LunarDetector::RELEASE_PARAM));
        y += rowPitch;
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(centerX, y)), module, LunarDetector::ENV_OUTPUT));
        y += rowPitch;
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(centerX, y)), module, LunarDetector::GATE_OUTPUT));
    }

    void step() override {
        syncPanelTheme(this, "LunarDetector", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        appendAmbientThemeMenu(menu);
    }
};

Model* modelLunarDetector = createModel<LunarDetector, LunarDetectorWidget>("LunarDetector");
