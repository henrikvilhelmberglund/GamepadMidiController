# GamepadMidiController (Steam Controller fork)

Turn a **Steam Controller** (with the "Puck" wireless receiver, PID 0x1304) into a full-featured MIDI controller for any DAW. Heavy fork of the [upstream project](https://github.com/romandykyi/GamepadMidiController) — rewritten to talk directly to the controller via [libusb/hidapi](https://github.com/libusb/hidapi) instead of going through Steam Input / XInput, so you get the controller's full range (uncompressed sticks, both trackpads with pressure, capacitive touch, grip buttons, gyro + accelerometer) without Steam running.

Outputs:
- MIDI CCs for sticks, trackpads, triggers, gyro, accelerometer, every button
- Pitch bend from triggers (LT down, RT up, center at rest)
- AccX-tilt → mod wheel (CC 1) with deadzone
- AccY-tilt → note velocity (flat = loud, tilted up = soft)
- MIDI note_on / note_off across four playing modes (see below)

Everything is driven from one compile-time config file: [GamepadMidiController/mappings.h](GamepadMidiController/mappings.h).

## Requirements

- A Steam Controller + Puck wireless receiver (PID 0x1304)
- **Steam must be closed** at runtime — Steam grabs the controller's HID device exclusively, on every OS.
- A virtual MIDI port:
  - **Windows**: [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html)
  - **macOS**: built-in IAC Driver (enable a port in Audio MIDI Setup → MIDI Studio → IAC Driver)
  - **Linux**: ALSA virtual MIDI (`sudo modprobe snd-virmidi`) or use JACK / `aconnect`
- C++20 compiler, CMake ≥ 3.14
  - Windows: MSVC 2022+ tested
  - macOS: AppleClang 15+
  - Linux: GCC 11+ or Clang 14+

## Build

### Windows

From a **Visual Studio x64 Developer Command Prompt** in the repo root:

```cmd
cmake --preset x64-release
cmake --build out\build\x64-release --config Release
```

Or in VS Code: Ctrl+Shift+B uses `.vscode/tasks.json`, which also has a "Build and Run" task that spawns the exe in its own external cmd window.

Output: `out\build\x64-release\GamepadMidiController\GamepadMidiController.exe`

### Linux / macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output: `build/GamepadMidiController/GamepadMidiController`

On **Linux** you'll likely also need:
- Install `libudev-dev` (`sudo apt install libudev-dev` on Debian/Ubuntu; the equivalent on Fedora/Arch).
- Add a udev rule so non-root processes can open the Steam Controller's HID device. Save the following as `/etc/udev/rules.d/99-steam-controller.rules`, then `sudo udevadm control --reload-rules && sudo udevadm trigger`:

  ```
  # Steam Controller "Puck" wireless receiver (PID 0x1304)
  SUBSYSTEM=="hidraw", ATTRS{idVendor}=="28de", ATTRS{idProduct}=="1304", MODE="0666"
  KERNEL=="uinput", MODE="0666"
  ```

On **macOS** you may be prompted to grant Input Monitoring permission on first run (System Settings → Privacy & Security → Input Monitoring). The build is not signed/notarized; just allow it through.

## Run

1. Close Steam.
2. Plug in the Puck dongle, power on the controller.
3. Create / enable a virtual MIDI port (see Requirements above for OS-specific tooling).
4. Run the exe. Pick the virtual MIDI port from the list when prompted. If Steam is still running or the controller is asleep, it shows what's wrong and waits (Q to quit) instead of exiting. The dashboard also flags when the controller stops sending input mid-session.
5. In your DAW: enable the virtual port as an input, arm a track, and play.

Dashboard shows every input live: stick/pad positions, gyro/accel, button states, mod wheel value, velocity, current scale and chord-mode state.

## Note-playing modes

Switch by focusing the console and pressing `1`, `2`, `3`, or `4`.

### Mode 1 — DIATONIC
Fixed C-major scale on the D-pad + face buttons. Modifiers (LB/RB/L4/R4/L5/R5) are momentary semitone shifts; releasing a face button while a modifier is held sustains the note.

### Mode 2 — CHROMATIC
Monophonic with a drifting root. Every press plays (root + button's chromatic offset) and **advances the root to that note**. Subsequent presses while one is held shift the active note without advancing root. Releasing the last finger snaps root to the most recently played note. Modifiers (LB/RB/L4/R4/L5/R5) are *also* note-playing buttons here. `R3` (right-stick click) resets root to C. `L3` snaps to the saved "home" root. `QAM` toggles root between C and the alt root. `Steam` saves current root as home + alt.

### Mode 3 — SCALE
Polyphonic, fixed (no root drift). Each button plays (tonic + scale-step offset) in the **currently-selected scale**. `L3`/`R3` cycles through 13 scales (Major, Minor, all five remaining diatonic modes, PentMaj/PentMin/Blues, plus Hirajoshi/In/Insen for Japanese flavor). `QAM` toggles tonic between C and the alt; `Steam` saves alt.

### Mode 4 — CHORD
Polyphonic. Each button plays a chord. Two sub-modes:
- **Default**: button N's chord is the diatonic triad rooted at scale-step `SCALE_OFF_N` (so different scale degrees give different chord qualities automatically — I-Maj, ii-min, iii-min, IV-Maj, V-Maj, vi-min, vii-dim in major).
- **Palette (QAM toggled on)**: a curated composition palette of functional chords (I/V/IV/vi on the face buttons, ii/iii/V7/vii° on the d-pad, sevenths and suspensions in the extras). In palette mode, `L3`/`R3` shifts the key by perfect fifths (circle-of-fifths style).

In both sub-modes, `L3`/`R3` (default sub-mode) cycles scale, and the chord qualities adapt automatically.

## DAW mode

A separate mode for driving the DAW, e.g. while recording audio with an instrument in your hands. It is on when the app starts; toggle it with `D` in the console. While DAW mode is on, the controller sends only DAW commands: no notes, no CCs.

| Button | Action | Button | Action |
|--------|--------|--------|--------|
| **Left pad click** | **Record take** (see below); again to stop | DPad ← / → | −1 / +1 bar |
| **Right pad click** | Play / stop (like spacebar); stops a take too | LB / RB | −N / +N bars (N = 4 by default) |
| A / B | Play-stop / stop | DPad ↑ / ↓ | Select the track above / below |
| Y | Undo | R3 | Add cue marker |
| Menu | Plain record toggle | L3 | Go to start |
| View | Metronome on/off | QAM | Loop on/off |
| L4 / R4 | Select the clip to the left / right on the track | R5 | Select the clip under the play cursor (click any clip on that track once first) |

Change the layout in the `DAW_BINDINGS` table in [mappings.h](GamepadMidiController/mappings.h).

Each command is a CC on **MIDI channel 16** whose number is the action (`mappings::DawAction`). If an output port with "DAW" in its name exists, the commands go there instead of the main port.

### Bitwig setup

1. In loopMIDI, add a second port called `loopMIDI DAW` (recommended; keeps DAW commands away from your instrument input).
2. Copy [bitwig/GamepadMidiController.control.js](bitwig/GamepadMidiController.control.js) into `Documents/Bitwig Studio/Controller Scripts/GamepadMidiController/`.
3. Bitwig → Settings → Controllers → Add controller → *GamepadMidiController* → *Steam Controller DAW*, input = `loopMIDI DAW`.

### Record take: back-to-back audio takes on one track

Click the left pad to start a take: playback starts **pre-roll** beats (default 2) before the cursor so you get a lead-in. Click it again (or the right pad) to stop. The cursor then returns to where the take started, so the next click redoes the same section; jump ahead with LB/RB to move on. Every take runs with Arranger Overdub on, so recording over an existing clip adds a comp lane instead of cutting it. Three take modes (controller preferences → *Take mode*):

- **Punch in at the cursor** (default): recording starts an **early margin** (default 80 ms) before the cursor and runs until you stop. The pre-roll is only a lead-in and never covers the previous take. Anything played before the punch-in point isn't recorded (Bitwig discards it).
- **Record everything**: recording starts with the pre-roll, so nothing you play is lost, but the pre-roll covers the end of the previous take until you swipe the comp boundary.
- **Punch in/out at markers**: like punch-in, plus a cue marker at each take's start and end; a take punches out at the next marker so it can't run into material after it, even if you stop late. This is the only mode that adds markers.

Bitwig's punch points are the arranger loop brace, so the punch modes move the loop brace during a take and restore your loop and punch settings afterwards.

**Selecting takes:** L4 / R4 select the clip to the left / right, R5 selects the clip under the play cursor. They step from the current selection (like the arrow keys), so click one clip on the track in the arranger first; moving the cursor with the controller or the ruler keeps that selection.

Take mode, pre-roll, early margin, overdub, the bar count for big jumps and where the cursor goes after a take ("Take start" / "Next bar after stop") are under the controller's preferences in Bitwig.

For other DAWs, MIDI-learn the channel-16 CCs (press `L` first, see below).

### Debugging the Bitwig script

The script listens for SysEx debug commands (`F0 7D <ASCII command> F7`) on its MIDI input. These can invoke any Bitwig action by id, move the cursor, or log the transport, the selected clip and the script's state to `Documents/Bitwig Studio/GamepadMidiController-debug.txt`. The command list is at the debug-hook comment in [the script](bitwig/GamepadMidiController.control.js). Never write files into the script's own folder: Bitwig reloads the script whenever anything there changes, which loses a take in progress.

### Learn mode

Press `L` to mute every continuous/ambient CC (sticks, pads, triggers, gyro, accel, tilt, pad pressure, capacitive touch), so a DAW's MIDI learn only sees the button you press. Press `L` again when done.

## MIDI output (default)

| What | CCs |
|------|-----|
| Trackpad XY (L/R) | CC 41–44 — **Surge XT macros 1–4** by default |
| Stick XY (L/R) | CC 45–48 — **Surge XT macros 5–8** |
| Trigger L / R (individual) | CC 5 / 6 |
| Mod wheel from AccX tilt | CC 1 |
| Pitch bend from triggers | pitch bend message |
| Gyro X/Y/Z | CC 70 / 71 / 72 |
| Accel X/Y/Z | CC 73 / 74 / 75 |
| Standard buttons (A/B/X/Y/LB/RB/DPad/Start/Select/Guide) | CC 20–34 |
| Triton-specific (touches, grip, paddles, etc.) | CC 102–118 |

Full map and customization notes in [CC_MAP.md](CC_MAP.md).

## Customization

Edit [GamepadMidiController/mappings.h](GamepadMidiController/mappings.h) and rebuild. Every CC number, MIDI note, scale interval, chord voicing, modifier delta, deadzone, scaling factor, and per-button offset is a one-line `constexpr` there. The file is organized by section with comments — no code restructuring needed for typical remapping.

Some highlights you can tune:
- `SCALE_OFF_*` — per-button scale-step offset (controls SCALE + CHORD default sub-mode)
- `SCALES[]` — add or remove scales; each is `{name, count, intervals}`
- `COMP_CHORDS[]` — composition palette (which chord on which button in CHORD palette mode)
- `ACCY_VELOCITY_DEADZONE` / `ACCY_VELOCITY_MAX_RAW` — tilt-velocity sensitivity
- `ACCX_DEADZONE` / `ACCX_MAX` — mod-wheel tilt sensitivity
- `GYRO_ACCEL_BOOST` — gyro/accel CC scaling multiplier
- `FREEZE_TRACKPADS_ON_RELEASE` — whether trackpad CCs freeze on finger-lift

## Steam Controller HID protocol notes

The Triton/Puck speaks a different HID protocol from the original Steam Controller (different report ID, different feature-report byte length, no shared init sequence). See [HIDAPI_MIGRATION_PLAN.md](HIDAPI_MIGRATION_PLAN.md) for what was reverse-engineered and where. [SDL3_MIGRATION_PLAN.md](SDL3_MIGRATION_PLAN.md) and [STEAMWORKS_MIGRATION_PLAN.md](STEAMWORKS_MIGRATION_PLAN.md) document alternative implementation paths that were considered but not taken.

## Used libraries

- [celtera/libremidi](https://github.com/celtera/libremidi) — MIDI output
- [libusb/hidapi](https://github.com/libusb/hidapi) — direct HID access to the Steam Controller
