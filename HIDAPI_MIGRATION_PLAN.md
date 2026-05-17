# Direct HIDAPI Migration Plan

Drop `libgamepad` entirely and talk to the Steam Controller's HID interface directly using [libusb/hidapi](https://github.com/libusb/hidapi). This gives full hardware access — sticks, triggers, trackpads (X/Y/pressure/click), grip buttons, **gyro + accelerometer** — without any dependency on Steam Input, Steamworks, or SDL.

**Trade-off**: more code than the Steam-mediated options, but every line is ours and there are mature open-source reference implementations to crib from.

## References (read these before / during implementation)

These are the definitive sources for the Steam Controller's HID protocol:

- **SDL2/3's existing driver**: [SDL_hidapi_steam.c](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam.c). Valve-contributed, in C, MIT-licensed. Covers the protocol up to (but not including) trackpads/gyro until the May 2026 PR — but the *protocol foundations* (device init, lizard mode, input report layout for buttons/sticks/triggers) have been here for years and are battle-tested.
- **SDL PR #15528**: [the May 2026 merge](https://github.com/libsdl-org/SDL/pull/15528). The diff shows exactly how to parse trackpads, grip, gyro from the SC's input report. Read this as the protocol-truth for the new features.
- **kozec/sc-controller**: https://github.com/kozec/sc-controller. Linux GUI driver in Python/C. Most readable, well-commented protocol implementation.
- **ynsta/steamcontroller**: https://github.com/ynsta/steamcontroller. Pure Python reference, easy to skim.

## Steam Controller hardware quick facts

- **Vendor ID**: `0x28de` (Valve).
- **Product IDs**:
  - Wired SC: `0x1102`.
  - Wireless dongle: `0x1142` — multiplexes up to **4 controllers** at different HID interfaces. We open each interface and check connection state.
- **HID interfaces per device**: multiple. Gamepad input is on the interface with usage page `0xFF00` (vendor-defined), NOT the keyboard/mouse interfaces. hidapi's `hid_enumerate` returns them all; filter by usage page.
- **Lizard mode**: by default, the SC sends keyboard + mouse HID reports (so it works as a desktop input device without a driver). We must **send a feature report to disable lizard mode** before we get the raw gamepad input report. (Feature report ID 0x81, payload depends on the controller — see SDL source.)
- **Gyro**: must be **explicitly enabled** via a feature report after disabling lizard mode. Gyro then comes through in each input report as a quaternion + raw accelerometer triple.
- **Input report**: ~64 bytes, sent at ~60 Hz when in gamepad mode (~100 Hz with gyro enabled).

## Phase 1 — Plumbing (small, demonstrable)

Goal: open the SC, disable lizard mode, dump raw input report bytes to the console. Proves we can talk to the hardware.

1. **[GamepadMidiController/CMakeLists.txt](GamepadMidiController/CMakeLists.txt)**:
   - Remove the `libgamepad` `FetchContent_Declare`.
   - Add hidapi:
     ```cmake
     FetchContent_Declare(
         hidapi
         GIT_REPOSITORY https://github.com/libusb/hidapi.git
         GIT_TAG hidapi-0.14.0
     )
     FetchContent_MakeAvailable(hidapi)
     target_link_libraries(GamepadMidiController PRIVATE hidapi::hidapi)
     ```
   - On Windows, hidapi uses the Win32 HID API — no extra deps.
2. **New file [GamepadMidiController/steam_controller.h/.cpp](GamepadMidiController/)**: encapsulates everything HID-specific:
   - `bool open_first()` → enumerates HID devices, finds one matching Valve VID + SC PID + usage page `0xFF00`, opens it.
   - `bool disable_lizard_mode()` → sends the magic feature report.
   - `bool enable_gyro()` → sends gyro-enable feature report.
   - `std::optional<RawReport> read()` → blocking read of next input report; returns the raw 64-byte buffer.
   - Destructor closes hidapi handle.
3. **Update [GamepadMidiController/GamepadMidiController.cpp](GamepadMidiController/GamepadMidiController.cpp)** `main()`:
   - Call `hid_init()`.
   - Open the SC.
   - Disable lizard mode + enable gyro.
   - In a loop, `read()` and print bytes as hex to the console.
   - Wiggle sticks / press buttons → confirm we see byte changes in expected positions.

**Phase 1 is a hard checkpoint.** If we can't open the device or get raw reports, every subsequent phase is broken. The user should:
- Close Steam fully (Task Manager → kill `steam.exe` if needed — Steam re-grabs HID devices when running).
- Run the new exe.
- See a stream of hex bytes changing as the controller is moved.

## Phase 2 — Parse the input report

Goal: extract structured controller state from the raw bytes.

Define a struct mirroring the SC input report layout:
```cpp
struct SCState
{
    // Buttons (bitfields)
    bool a, b, x, y;
    bool lb, rb;
    bool lt_click, rt_click;     // triggers register a digital "click" at full press
    bool back, start, guide;
    bool lstick_click;            // SC has only one stick (left); click is a button
    bool lpad_click, rpad_click;  // trackpad clicks
    bool lgrip, rgrip;            // grip buttons

    // Analog
    int16_t lstick_x, lstick_y;   // -32768..32767 (only present when stick is pressed/used)
    uint8_t ltrigger, rtrigger;   // 0..255
    int16_t lpad_x, lpad_y;       // trackpad position when finger is on it
    int16_t rpad_x, rpad_y;
    uint8_t lpad_pressure;        // 0..255
    uint8_t rpad_pressure;

    // Capacitive touch (which surface is being touched RIGHT NOW)
    bool lpad_touched;
    bool rpad_touched;

    // Motion (gyro + accel)
    int16_t accel_x, accel_y, accel_z;     // raw accelerometer
    int16_t gyro_x, gyro_y, gyro_z;        // angular rates (pitch/yaw/roll rates)
    int16_t gyro_quat_w, gyro_quat_x,      // optional: fused quaternion (firmware-fused)
            gyro_quat_y, gyro_quat_z;
};

bool parse_input_report(const uint8_t* buf, size_t len, SCState& out);
```

Byte offsets and bit layouts come from the SDL `SDL_hidapi_steam.c` source and PR #15528 — copy them carefully, cite the source. Don't reverse-engineer from scratch; the references already did the hard work.

## Phase 3 — Wire to MIDI

Keep the existing MIDI sending code, but feed it from `SCState` instead of libgamepad's event callbacks. The shape of the work:

1. Maintain a `prev_state` and a `current_state`.
2. After each `read()`, parse into `current_state`.
3. For each digital field: if it differs from `prev_state`, send CC (127 on press, 0 on release) using the same CC numbers from [CC_MAP.md](CC_MAP.md).
4. For each analog field: scale to 0..127, send CC. Optional optimization: only send if changed by ≥1 (skip identical reports).
5. Copy `current_state` into `prev_state` and loop.

### New CCs to add for SC-specific inputs

Pulled from the same numbering as [SDL3_MIGRATION_PLAN.md](SDL3_MIGRATION_PLAN.md) and [STEAMWORKS_MIGRATION_PLAN.md](STEAMWORKS_MIGRATION_PLAN.md):

| CC | Input                  | Type     |
|----|------------------------|----------|
| 10 | Left trackpad X        | bipolar  |
| 11 | Left trackpad Y        | bipolar  |
| 12 | Left trackpad pressure | unipolar |
| 13 | Right trackpad X       | bipolar  |
| 14 | Right trackpad Y       | bipolar  |
| 15 | Right trackpad pressure| unipolar |
| 16 | Left trackpad click    | digital  |
| 17 | Right trackpad click   | digital  |
| 35 | Left grip              | digital  |
| 36 | Right grip             | digital  |
| 37 | Left trackpad touch    | digital  |
| 38 | Right trackpad touch   | digital  |
| 40 | Gyro pitch rate        | bipolar  |
| 41 | Gyro yaw rate          | bipolar  |
| 42 | Gyro roll rate         | bipolar  |
| 43 | Accel X                | bipolar  |
| 44 | Accel Y                | bipolar  |
| 45 | Accel Z                | bipolar  |

Gyro and accel raw values are `int16_t` — need to decide scaling. Reasonable default: clamp to ±max-expected-rate (e.g. 2000 dps for gyro, ±2g for accel) and map to 0..127 with 64 = center. Maybe expose this scaling as a constant for tuning.

## Phase 4 — Polish

Keep the existing live dashboard but expand it for the new inputs. Probably needs ~12 more lines.

Other niceties (some of these are in [CC_MAP.md](CC_MAP.md) "Expansion ideas"):

- **Filter zero gyro**: when the controller is still, gyro reports near-zero with small jitter. Add a deadzone (e.g. `|rate| < 100` → 0) to avoid spamming Reaper with tiny CC values.
- **Gyro integration mode**: instead of "rate of rotation" → CC, integrate over time to get absolute orientation → CC. Better for "tilt = filter cutoff" style mappings.
- **Trackpad relative mode**: trackpad delta X/Y as +1/-1 increments instead of absolute position. Pairs with Reaper's relative CC parameters.
- **Modal layers**: hold a grip button to switch to a second CC map.

## Risks / gotchas

1. **Steam must be closed**. Steam grabs the SC's HID device exclusively. This is not a bug we can engineer around — it's Windows HID exclusivity + Steam's design. Document loudly in [README.md](README.md). If `hid_open` returns nullptr, "is Steam running?" is the first thing to check.
2. **Lizard mode kicks back in**: after the controller is idle for a while, or if it disconnects/reconnects, it may revert to lizard mode and start spamming keyboard/mouse events to the OS. Re-send the disable feature report on reconnect.
3. **Wireless dongle quirks**:
   - Dongle exposes 4 HID interfaces (one per controller slot). Most are "not connected" until a controller pairs to that slot. Iterate and pick the one reporting "connected".
   - Battery state and connection state come through as separate event reports — handle those gracefully.
4. **Firmware variance**: protocol details have evolved across SC firmware versions. SDL's driver handles a few variants. Test on the actual firmware version you're running; if reports look wrong, check `device->version_number` and cross-reference SDL's per-firmware quirks.
5. **HID report ID vs no report ID**: hidapi on Windows includes the report ID as byte 0 of the buffer; on Linux it strips it. Code defensively (check `buf[0] == expected_id`).
6. **Threading**: `hid_read` is blocking. Either:
   - Put the HID read loop in its own thread, push parsed events onto a queue consumed by `main`, OR
   - Use `hid_read_timeout(handle, buf, len, 1)` in a tight loop so `main` can also handle other things.
   The first is cleaner.
7. **Hot plug**: hidapi doesn't natively notify of device add/remove. Detect by failed `hid_read` → close + re-enumerate → reopen. Loop forever.

## Open questions to resolve at implementation time

- Exact feature report bytes for "disable lizard mode" + "enable gyro" — copy from SDL's `SDL_hidapi_steam.c`, don't guess.
- Whether the wired SC and dongle-paired SC have identical input report layouts (the references suggest yes, with minor differences in header bytes).
- Whether we want quaternion-fused orientation or raw gyro rates as the primary "gyro" output. Probably both — let the user pick via a CC range.

## Done when

- Exe runs (Steam closed) and the live dashboard fills in for **every** input on the SC: A/B/X/Y/LB/RB, stick X/Y, both triggers, both trackpads X/Y/pressure/click/touch, both grip buttons, dpad equivalents, start/back/guide, gyro pitch/yaw/roll, accel X/Y/Z.
- Sweeping any axis to its physical extreme makes the dashboard's `seen=[min,max]` hit `[0,127]`. (Gone: the Steam Input range compression that started this whole thread.)
- All CCs from [CC_MAP.md](CC_MAP.md) work, plus the new ones from Phase 3 above.
- [README.md](README.md) updated: "Close Steam before launching" + "This builds directly against hidapi — no Steam dependency at runtime."
- [CC_MAP.md](CC_MAP.md) updated with the new CCs.

## Effort estimate

- **Phase 1**: 1–2 hours. Mostly CMake + hidapi boilerplate + the lizard-mode feature report. Demonstrable at end.
- **Phase 2**: 3–5 hours. Mechanical byte-offset work cross-referencing SDL's source. Bugs likely; testing with real hardware iterative.
- **Phase 3**: 2–3 hours. Mostly straightforward state-diff → CC sending.
- **Phase 4 (polish)**: 1–2 hours.

Total: **roughly a long day or split across two evenings**. Most of the risk is in Phase 2; if SDL's source is followed carefully, very tractable.
