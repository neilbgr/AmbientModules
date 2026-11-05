#!/usr/bin/env python3
"""Generate Lunar50Drone's factory chord presets (presets/Lunar50Drone/).

Usage: scripts/generate-lunar50drone-presets.py   (from anywhere)

Writes 19 standard chords on each of the 12 roots (C3 to B3), each in two
voicings (456 presets), as Voicing > Root > Group > Chord sub-menus:
  - Full (5 oscillators): bass root an octave below, then the chord from
    the root; triads add the upper octave, 5-note chords drop the upper
    root (bass + 3, 5, 7, 9). C major = C2 C3 E3 G3 C4.
  - Close (chord notes only): the chord in close position from the root,
    with only as many oscillators active as it has notes.

A preset only sets the 5 oscillators' frequency, ON and MOD (always on, so
a V/oct keyboard on Frequency CV transposes the whole chord from there).
It has no "data" key, so the envelope, Volt, Hold, attenuverter, Tone,
oscillator mix and FM topology the user has set stay as they are.

The whole presets/Lunar50Drone/ folder is deleted and rewritten, so edit
the tables below, not the generated files.
"""
import json
import shutil
from pathlib import Path

NUM_OSC = 5
FREQ_PARAM, ACTIVE_PARAM, MOD_PARAM = 0, 5, 10  # Lunar50Drone::ParamIds
ROOT = -1.0  # FREQ_PARAM is in octaves relative to C4: C3

# Spelled the most common way (flats, except F#); semitones above C3.
ROOTS = ["C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]

# (sub-menu, [(name after the root, semitones above the root)])
CHORDS = [
    ("Triads", [
        (" major", [0, 4, 7]),
        (" minor", [0, 3, 7]),
        (" dim", [0, 3, 6]),
        (" aug", [0, 4, 8]),
        (" sus2", [0, 2, 7]),
        (" sus4", [0, 5, 7]),
        ("5 (power)", [0, 7]),
    ]),
    ("Sevenths", [
        ("maj7", [0, 4, 7, 11]),
        ("7", [0, 4, 7, 10]),
        ("m7", [0, 3, 7, 10]),
        ("m7b5", [0, 3, 6, 10]),
        ("dim7", [0, 3, 6, 9]),
        ("mMaj7", [0, 3, 7, 11]),
    ]),
    ("Sixths and added", [
        ("6", [0, 4, 7, 9]),
        ("m6", [0, 3, 7, 9]),
        ("add9", [0, 4, 7, 14]),
    ]),
    ("Ninths", [
        ("9", [0, 4, 7, 10, 14]),
        ("maj9", [0, 4, 7, 11, 14]),
        ("m9", [0, 3, 7, 10, 14]),
    ]),
]

VOICINGS = [("Full (5 oscillators)", "full"), ("Close (chord notes only)", "close")]


def full_voicing(notes):
    """Semitones above the root for all 5 oscillators."""
    if len(notes) == 2:
        return [-12, -5, 0, 7, 12]  # power chord: C2 G2 C3 G3 C4
    if len(notes) == 3:
        return [-12] + notes + [12]
    if len(notes) == 4:
        return [-12] + notes
    return [-12] + notes[1:]  # 5 notes: bass root + 3, 5, 7, 9


def preset(version, semitones):
    """semitones (above C3): one per active oscillator, the rest are inactive."""
    params = []
    for i in range(NUM_OSC):
        active = i < len(semitones)
        # An inactive oscillator (Close only) stays on the root, the first
        # note, so turning it on (button or trigger) adds an in-tune note.
        st = semitones[i] if active else semitones[0]
        params.append({"id": FREQ_PARAM + i, "value": ROOT + st / 12.0})
    for i in range(NUM_OSC):
        params.append({"id": ACTIVE_PARAM + i, "value": 1.0 if i < len(semitones) else 0.0})
    for i in range(NUM_OSC):
        params.append({"id": MOD_PARAM + i, "value": 1.0})
    return {"plugin": "AmbientModules", "model": "Lunar50Drone", "version": version, "params": params}


def main():
    root = Path(__file__).resolve().parent.parent
    version = json.loads((root / "plugin.json").read_text(encoding="utf-8"))["version"]
    out = root / "presets" / "Lunar50Drone"
    if out.exists():
        shutil.rmtree(out)
    count = 0
    for v, (voicing_name, kind) in enumerate(VOICINGS, 1):
        for r, root_name in enumerate(ROOTS):
            for g, (group, chords) in enumerate(CHORDS, 1):
                folder = out / f"{v:02d}_{voicing_name}" / f"{r + 1:02d}_{root_name}" / f"{g:02d}_{group}"
                folder.mkdir(parents=True)
                for c, (name, notes) in enumerate(chords, 1):
                    semitones = [n + r for n in (full_voicing(notes) if kind == "full" else notes)]
                    text = json.dumps(preset(version, semitones), indent=2) + "\n"
                    (folder / f"{c:02d}_{root_name}{name}.vcvm").write_text(text, encoding="utf-8")
                    count += 1
    print(f"Wrote {count} presets to {out.relative_to(root)}")


if __name__ == "__main__":
    main()
