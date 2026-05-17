# SDL3 Migration Plan

Switch the project from `libgamepad` to **SDL3** so the Steam Controller can be read directly via HIDAPI instead of going through Steam Input's XInput emulation. Wait until SDL3 ships a tagged release that contains the Steam Controller HIDAPI work (PR [libsdl-org/SDL#15528](https://github.com/libsdl-org/SDL/pull/15528), merged 2026-05-14) before starting.

## Why migrate

Current setup forces Steam Input to translate the Steam Controller to XInput. That compresses axis ranges (e.g. CC2 only reaching ~50–80 instead of 0–127), requires launching this exe through Steam, and hides the trackpads + grip buttons + gyro entirely.

SDL3 with the merged PR talks to the controller's HID directly:
- Full, uncompressed axis range.
- Trackpads exposed as analog X/Y + click + pressure (left and right).
- Grip buttons exposed as real buttons.
- Gyro (already in SDL3) usable as 3 continuous CCs.
- Capacitive touch detection on the sticks.
- No need to launch through Steam.

## Prerequisite: wait for a release

The PR merged into SDL's `main` on 2026-05-14. No tagged SDL3 release contains it yet. **Check https://github.com/libsdl-org/SDL/releases — start migration when a release ≥ the first one that includes commit from PR #15528 is available.**

If urgency strikes before then, the alternative is to pin FetchContent to a `main` commit hash (works but no auto-updates).

## Phase 1 — Minimum viable swap (same CC map as today)

Goal: build with SDL3, verify Steam Controller is detected via SDL's HIDAPI path, keep the existing CC numbers working unchanged.

1. **Update [GamepadMidiController/CMakeLists.txt](GamepadMidiController/CMakeLists.txt)**:
   - Remove the `libgamepad` `FetchContent_Declare`.
   - Add SDL3 via `FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-X.Y.Z)`.
   - Replace `target_link_libraries(... gamepad)` with `target_link_libraries(... SDL3::SDL3)`.
   - Confirm `SDL_HIDAPI` is enabled (it's on by default in SDL3).
2. **Update [GamepadMidiController/GamepadMidiController.h](GamepadMidiController/GamepadMidiController.h)**:
   - Replace `#include <libgamepad.hpp>` with `#include <SDL3/SDL.h>`.
3. **Rewrite [GamepadMidiController/GamepadMidiController.cpp](GamepadMidiController/GamepadMidiController.cpp)**:
   - In `main()`: call `SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)`, then in a loop call `SDL_PollEvent(&ev)` and dispatch:
     - `SDL_EVENT_GAMEPAD_ADDED` → `SDL_OpenGamepad(ev.gdevice.which)`.
     - `SDL_EVENT_GAMEPAD_REMOVED` → close the gamepad.
     - `SDL_EVENT_GAMEPAD_BUTTON_DOWN` / `_UP` → call current `button_handler` equivalent.
     - `SDL_EVENT_GAMEPAD_AXIS_MOTION` → call current `axis_handler` equivalent. SDL axis values are `int16_t` (-32768..32767 for sticks, 0..32767 for triggers) — scale to 0..127.
   - Replace the `gamepad::button::*` switch with `SDL_GAMEPAD_BUTTON_SOUTH/EAST/WEST/NORTH/...` (SDL3 renamed A/B/X/Y to compass directions).
   - Replace the `gamepad::axis::*` switch with `SDL_GAMEPAD_AXIS_LEFTX/LEFTY/RIGHTX/RIGHTY/LEFT_TRIGGER/RIGHT_TRIGGER`.
   - Delete the libgamepad event-handler callbacks; everything moves into the main poll loop.
   - Keep the live dashboard logic — only its data source changes.
4. **Verify Steam isn't intercepting**: in Steam → Settings → Controller, **disable "Steam Input for Steam Controller"** globally before running. If Steam Input is enabled, SDL won't get HIDAPI access — it'll see the XInput emulation instead, defeating the migration. Test with Steam fully closed first, then with Steam open and Steam Input disabled for the SC.
5. **Confirm CC parity**: A/B/X/Y/LB/RB/sticks/triggers/dpad/start/back should all emit the same CCs as before. The dashboard's `seen=[min,max]` should now reach `[0,127]` on full physical sweep — that's the success signal.

## Phase 2 — Expose the new inputs

Once Phase 1 works, add the Steam Controller specifics. Pick CC numbers in a fresh range so the original map stays intact.

Suggested new CCs (continuous unless noted):

| CC | Input | Notes |
|----|-------|-------|
| 10 | Left trackpad X | bipolar (-1..+1 → 0..127), center=64 |
| 11 | Left trackpad Y | bipolar, center=64 |
| 12 | Left trackpad pressure | unipolar (0..1 → 0..127) |
| 13 | Right trackpad X | bipolar |
| 14 | Right trackpad Y | bipolar |
| 15 | Right trackpad pressure | unipolar |
| 16 | Left trackpad click | button (127 / 0) |
| 17 | Right trackpad click | button (127 / 0) |
| 35 | Left grip button | button (127 / 0) |
| 36 | Right grip button | button (127 / 0) |
| 40 | Gyro pitch | bipolar |
| 41 | Gyro yaw | bipolar |
| 42 | Gyro roll | bipolar |
| 37 | Left stick capacitive touch | button (127 = touched, 0 = not) |
| 38 | Right stick capacitive touch | button (127 / 0) |

Use these SDL3 APIs (verify names against SDL3 docs at migration time — these are accurate as of the PR's merge):
- Trackpads: `SDL_GetNumGamepadTouchpads`, `SDL_GetNumGamepadTouchpadFingers`, `SDL_GetGamepadTouchpadFinger(gp, touchpad, finger, &state, &x, &y, &pressure)`.
- Gyro / accelerometer: `SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true)` and read via `SDL_EVENT_GAMEPAD_SENSOR_UPDATE`.
- Grip buttons may map to `SDL_GAMEPAD_BUTTON_LEFT_PADDLE1` etc. — depends on how the SDL PR exposes them. Check the merged PR's button assignments (the merge message noted button mappings were deferred to PR #15601).

Also worth adding when convenient:
- Modal layer / shift: hold LB to remap A/B/X/Y to a second bank.
- Toggle vs momentary mode per button (for mute/solo/arm in Reaper).
- Encoder/relative mode for sticks (Reaper supports relative CCs natively).
- 14-bit CC for sticks for smoother filter sweeps.

(See [CC_MAP.md](CC_MAP.md) "Expansion ideas" section — those still apply on top of SDL3.)

## Risks / gotchas to retest at migration time

1. **Steam Input still hijacks the SC unless disabled** — even with SDL3, if Steam is running with Steam Input enabled for the Steam Controller, it'll claim the HID device exclusively. Plan: disable it in Steam's controller settings, document the requirement in [README.md](README.md).
2. **Wireless dongle quirks** — the original PR may not support every SC firmware revision or BLE-only mode. Test wired + dongle.
3. **SDL build cost** — SDL3 is significantly larger than libgamepad. Configure + first build goes from ~30s to a few minutes. Set `SDL_TEST_LIBRARY=OFF` and other CMake options to trim.
4. **Button name changes**: SDL3 uses `SDL_GAMEPAD_BUTTON_SOUTH/EAST/WEST/NORTH` instead of `A/B/X/Y`. South = A (bottom face button). Don't get this wrong — Y and Nintendo-style layouts can flip.
5. **Axis sign for Y**: SDL reports Y axes with positive = down. libgamepad inverts this internally. Decide whether to match the previous behavior (invert in code) or flip the CC direction in Reaper.
6. **Trackpad coordinates**: SDL trackpad X/Y are in 0..1 (unipolar), not -1..+1. The "Y" axis convention may differ from sticks — verify before mapping to bipolar CC.

## Done when

- Steam Controller is detected by the program with **Steam Input disabled** for the SC (and ideally also with Steam closed entirely).
- Dashboard `seen=[min,max]` reaches `[0,127]` on every axis after a full physical sweep — the original range-compression bug is gone.
- All Phase 2 inputs (trackpads, grip, gyro) appear as CCs and can be MIDI Learned in Reaper.
- [README.md](README.md) and [CC_MAP.md](CC_MAP.md) are updated for the new dep + new CC map.
