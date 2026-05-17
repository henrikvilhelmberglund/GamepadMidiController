# MIDI CC Map

**To customize this map**: edit [GamepadMidiController/mappings.h](GamepadMidiController/mappings.h) (every CC number, MIDI note, modifier delta, deadzone, etc. is a one-liner `constexpr` there) and rebuild. No code restructuring required for normal remapping.

All input is sent on **MIDI channel 1** by default (change `mappings::MIDI_CHANNEL` to use a different channel).

The map below is what the Steam Controller (Triton via Puck wireless receiver) sends when read via direct hidapi — no Steam, no SDL, no XInput emulation. See [HIDAPI_MIGRATION_PLAN.md](HIDAPI_MIGRATION_PLAN.md) for the implementation notes.

CC assignments avoid collisions with:
- **MIDI standard reserved CCs** (CC 1 = mod wheel, 7 = volume, 10 = pan, 64-95 = switches/pedals, 120-127 = channel mode)
- **Surge XT macro default CCs** (CC 41-48) — we use these *on purpose* for the eight most useful continuous inputs so Surge auto-maps them to macros 1-8.

## Surge XT macros — CC 41-48

These hit Surge XT's default macro CCs so no MIDI Learn is needed; macros 1-8 light up automatically.

| Surge macro | CC | Input | Range |
|-------------|----|-------|-------|
| Macro 1 | 41 | LSX (left stick X)   | bipolar, center 64 |
| Macro 2 | 42 | LSY (left stick Y)   | bipolar |
| Macro 3 | 43 | RSX (right stick X)  | bipolar |
| Macro 4 | 44 | RSY (right stick Y)  | bipolar |
| Macro 5 | 45 | LPX (left trackpad X)  | bipolar |
| Macro 6 | 46 | LPY (left trackpad Y)  | bipolar |
| Macro 7 | 47 | RPX (right trackpad X) | bipolar |
| Macro 8 | 48 | RPY (right trackpad Y) | bipolar |

## Triggers, pitch bend, mod wheel

| CC / msg | Input | Range |
|----------|-------|-------|
| 5        | LT (left trigger, individual)   | unipolar (0..127) |
| 6        | RT (right trigger, individual)  | unipolar |
| Pitch bend | RT bends up, LT bends down, center 8192 (14-bit MIDI pitch bend) | bipolar |
| CC 1     | Mod wheel from **AccX tilt** (deadzone ±3000 raw, so the controller can sit in your lap without spamming events). Magnitude of tilt → 0..127. | unipolar |

## Notes (MIDI note_on / note_off, FF14-style diatonic mapping)

D-pad and face buttons trigger MIDI notes on a C-major scale. The buttons still emit their CCs too (see CC 20-34 below) — note events are *additional*, not a replacement.

| Button | Note |
|--------|------|
| DPAD down  | C  (60) |
| DPAD right | D  (62) |
| DPAD left  | E  (64) |
| DPAD up    | F  (65) |
| A          | G  (67) |
| B          | A  (69) |
| X          | B  (71) |
| Y          | C+1 (72) |

Modifiers (all momentary holds — the total pitch offset is the sum of every modifier held). Each modifier state change retriggers any held notes, so rapid taps while a face button is held produce a stream of pitched note events.

| Modifier | Delta when held |
|----------|----------------|
| LB | -12 (octave down) |
| RB | +12 (octave up) |
| L4 | -1 (semitone down) |
| R4 | +1 (semitone up) |
| L5 | -2 (whole tone down) |
| R5 | +2 (whole tone up) |

Examples: hold LB + R4 = -11 semitones (octave down, then up a semitone). Hold L5 + R5 = 0 (they cancel).

## Standard buttons — CC 20-34 (127 on press, 0 on release)

| CC | Button     | CC | Button     |
|----|------------|----|------------|
| 20 | A          | 21 | B          |
| 22 | X          | 23 | Y          |
| 24 | LB         | 25 | RB         |
| 26 | L3 (left stick click)  | 27 | R3 (right stick click) |
| 28 | VIEW (back)| 29 | MENU (start) |
| 30 | DPAD up    | 31 | DPAD down  |
| 32 | DPAD left  | 33 | DPAD right |
| 34 | STEAM (guide) | | |

## Gyro + Accelerometer — CC 70-75

| CC | Input | Range |
|----|-------|-------|
| 70 | Gyro X (pitch rate)  | bipolar |
| 71 | Gyro Y (yaw rate)    | bipolar |
| 72 | Gyro Z (roll rate)   | bipolar |
| 73 | Accel X              | bipolar |
| 74 | Accel Y              | bipolar |
| 75 | Accel Z              | bipolar |

## Triton-specific inputs — CC 102-118 (MIDI "undefined" range, collision-free)

| CC  | Input | Type |
|-----|-------|------|
| 102 | Left trackpad pressure  | unipolar |
| 103 | Right trackpad pressure | unipolar |
| 104 | Left trackpad click  | button |
| 105 | Right trackpad click | button |
| 106 | Left trackpad touch (capacitive)  | button |
| 107 | Right trackpad touch (capacitive) | button |
| 108 | Left grip touch  | button |
| 109 | Right grip touch | button |
| 110 | Left stick touch  | button |
| 111 | Right stick touch | button |
| 112 | LT click (full pull) | button |
| 113 | RT click (full pull) | button |
| 114 | L4 paddle | button |
| 115 | L5 paddle | button |
| 116 | R4 paddle | button |
| 117 | R5 paddle | button |
| 118 | QAM (quick access menu) | button |

## Expansion ideas

1. **Modal layers / banks** — hold a paddle (L4/L5) to remap A/B/X/Y to a second bank of CCs. Doubles your knob count.
2. **Toggle vs momentary buttons** — make a button toggle (press = 127, next press = 0) instead of momentary. Better for mute/solo/arm in Reaper.
3. **Encoder/relative mode for trackpads** — instead of absolute X/Y, send +1 / -1 increments. Reaper supports relative CC modes natively.
4. **MIDI channel per region** — sticks on ch 1, buttons on ch 2, gyro on ch 3. Easier to filter in Reaper.
5. **Pitch bend / 14-bit CC** for sticks/trackpads — full 0-16383 resolution instead of 0-127. Smoother filter sweeps.
6. **Gyro orientation mode** — currently we send raw gyro *rates*. Integrating over time gives absolute orientation, which is nicer for "tilt = filter cutoff" mappings. The hardware also exposes a fused quaternion (we ignore it — TritonMTUFull's last 8 bytes).
7. **Gyro deadzone** — when the controller is still, gyro reports near-zero with small jitter. A deadzone (e.g. `|rate| < 100 → 0`) avoids spamming Reaper.

## Setup notes

- **Steam must be closed** before running. Steam grabs the controller's HID device exclusively — `hid_open` will return null until Steam releases it (closing Steam from the tray works; you may need Task Manager → kill `steam.exe` if it lingers).
- The program prompts for a MIDI output port at startup. Use [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html) to create a virtual MIDI port that Reaper can listen on.
- Lizard mode (the SC's default keyboard/mouse emulation) is auto-disabled on startup and refreshed every 3 seconds because the controller's firmware watchdog re-enables it periodically.
