# Changelog

## 2.5.3 (2026-11-13)
- LunarFilter, LunarSequencer, LunarPads, LunarJoystick: module bypass now passes signals through (see each module's note in the README).

## 2.5.2 (2026-11-05)
- Lunar50Drone: hardware-like tone, factory chord presets.
- Lunar50Drone: oscillator LEDs follow their oscillator; menu defaults.

## 2.5.1 (2026-11-03)
- LunarVCO: minBLEP anti-aliasing, hardware-like sub-oscillator and pulse width.
- All VCOs capped at 20 kHz.

## 2.5.0 (2026-10-31)
- Band-limited oscillators to reduce digital aliasing.
- LunarPapaSrapa: uncorrelated noise per channel and per instance.
- Lunar50Drone: oscillator buttons relabelled from MUTE to ON.
- Clearer tooltips for Lunar50Drone's modulation buttons and LunarPads.

## 2.4.1 (2026-10-17)
- LunarFilter: panel title tweak.

## 2.4.0 (2026-10-14)
- Hardened DSP; Polivoks-style filter self-oscillation in LunarFilter.
- Video patches shipped in `patches/`.

## 2.3.1 (2026-09-19)
- LunarSequencer: fixed CV range knob display/entry mismatch.

## 2.3.0 (2026-09-14)
- New modules: LunarFilter and LunarDetector.
- LunarMixer: stereo VU/peak level meter.
- Lunar50Drone: 2/3/4-oscillator-max mix modes.

## 2.2.0 (2026-09-06)
- New modules: LunarJoystick, LunarMixer and LunarPads.
- LunarVCO: OSC Out for hard sync, reworked panel.
- Solar 42f example patch.

## 2.1.2 (2026-08-29)
- Oscillator 1 active/modulated by default; envelope defaults aligned across Lunar50Drone, LunarVCO and LunarPapaSrapa.

## 2.1.1 (2026-08-23)
- LunarVCO: soft-clipped oscillator output.

## 2.1.0 (2026-08-23)
- Panel theme is now a single pack-wide setting.
- Fixed remaining polyphony gaps in Lunar50Drone, LunarVCO and LunarPapaSrapa.

## 2.0.1 (2026-08-18)
- LunarSequencer: Stages switch is click-to-target like a real toggle.

## 2.0.0 (2026-08-17)
- Initial release: Lunar50Drone, LunarVCO, LunarPapaSrapa, LunarLFO, LunarSequencer and Blank, with 5 panel themes.
