# MIDI CC Map

All input is sent on **MIDI channel 1** (change `MIDI_CHANNEL` in [GamepadMidiController.cpp](GamepadMidiController/GamepadMidiController.cpp) to use a different channel).

## Axes — continuous 0–127

| CC | Input | Range | Center |
|----|-------|-------|--------|
| 1  | LEFT_STICK_X   | bipolar (-1..+1 → 0..127) | 64 |
| 2  | LEFT_STICK_Y   | bipolar (-1..+1 → 0..127) | 64 |
| 3  | RIGHT_STICK_X  | bipolar (-1..+1 → 0..127) | 64 |
| 4  | RIGHT_STICK_Y  | bipolar (-1..+1 → 0..127) | 64 |
| 5  | LEFT_TRIGGER   | unipolar (0..1 → 0..127)  | 0  |
| 6  | RIGHT_TRIGGER  | unipolar (0..1 → 0..127)  | 0  |

## Buttons — 127 on press, 0 on release

| CC | Button     | CC | Button       |
|----|------------|----|--------------|
| 20 | A          | 27 | R_THUMB (right stick click) |
| 21 | B          | 28 | BACK         |
| 22 | X          | 29 | START        |
| 23 | Y          | 30 | DPAD_UP      |
| 24 | LB         | 31 | DPAD_DOWN    |
| 25 | RB         | 32 | DPAD_LEFT    |
| 26 | L_THUMB (left stick click) | 33 | DPAD_RIGHT |
|    |            | 34 | GUIDE        |

This is the full set of virtual inputs libgamepad's XInput backend exposes.

## Expansion ideas

Real ways to add more control surface without hardware changes:

1. **Modal layers / banks** — hold a "shift" button (e.g. LB) to remap A/B/X/Y to a second bank of CCs. Doubles your knobs without adding buttons.
2. **Toggle vs momentary buttons** — make a button toggle (press = 127, next press = 0) instead of momentary. Better for mute/solo/arm in Reaper.
3. **Encoder/relative mode for sticks** — instead of absolute 0..127, send +1 / -1 increments to act like an endless encoder (great for fine-tuning params). Reaper supports relative CC modes.
4. **MIDI channel per region** — sticks on ch 1, buttons on ch 2, etc. Easier to filter in Reaper.
5. **Pitch bend / 14-bit CC** for sticks — full 0–16383 resolution instead of 0–127. Nice for smooth filter sweeps.
6. **Steam Controller specifics** — the trackpads and gyro are not exposed through XInput. To get those you'd need to bypass Steam Input and read the controller directly (SDL2 supports the Steam Controller's native HID, or use the Steamworks API).
7. **Optional note mode** — keep CC for sticks, but optionally send notes for buttons (toggleable at startup). Useful for triggering drum samples.

## Steam Controller setup notes

The Steam Controller doesn't speak XInput natively — Steam Input has to translate. To make this work:

- Launch both the gamepad exe **and** Reaper as non-Steam games through Steam, so Steam Input attaches XInput emulation to both processes. Otherwise Steam's desktop config will hijack the inputs (mouse/keyboard) and they won't reach the gamepad exe.
- Alternative: set the Steam Controller's **Desktop Configuration** in Steam (Big Picture → Controller Settings → Desktop Configuration) to "Gamepad with Joystick Trackpad" or a blank/empty config, so it emits gamepad input system-wide without Steam intercepting it.
