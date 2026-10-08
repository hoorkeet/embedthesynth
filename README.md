# embedthesynth

A headless, GUI-less fork of the open-source Vital synthesizer engine, modified to run several instances on a Raspberry Pi 5 and to load presets by MIDI
program change.

This project is a modified version of Vital by Matt Tytel and is licensed under the GNU General Public License v3.0 (see LICENSE). It is not affiliated with, endorsed by, or supported by Vital Audio or the original authors. "Vital" is a trademark of its owner.

## Modifications
Changes from upstream (see commit history for details):
- Removed the GUI; sound generation only
- Preset loading via MIDI program change
Planned:
- Support for multiple engine instances
- Build changes for Raspberry Pi 5 (ARM64)
For details check this project commit history.

## Performance note
CPU load depends heavily on the preset. Presets with high unison, oversampling or many active voices can cause audio dropouts on a Raspberry Pi 5, especially with several instances running. It is up to user to choose presets accordingly.

## Building & run
make headless_server 2>&1 | grep -E "error|Error" | head -20

Put your *.vital files in patches dir, each should start with 3 digits, marking the program number e.g. 005_bass.vital or "007 bond vibe.vital". Connect external midi controller.

./headless/builds/linux/build/vital --live --patches patches --port-name BASS --midi-inputs Akai

In case your controller doesn't support - send prog change from another bash: python3 -c "import sys; sys.stdout.buffer.write(b'MThd\x00\x00\x00\x06\x00\x00\x00\x01\x00\x60MTrk\x00\x00\x00\x07\x00\xc0\x00\x00\xff\x2f\x00')" | aplaymidi -p 128:0 -

## Original Vital README
See ORIGINAL-VITAL-README.md