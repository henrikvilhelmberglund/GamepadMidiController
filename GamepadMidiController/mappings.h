// ============================================================================
//                          USER-EDITABLE MAPPINGS
// ============================================================================
//
// Edit this file to change CC numbers, MIDI note assignments, modifier deltas,
// or scaling parameters. Rebuild (Ctrl+Shift+B) to apply.
//
// Every value here is a compile-time constant - no runtime config to parse,
// just plain `constexpr`. Search for the section you care about:
//
//   * MIDI_CHANNEL          - which MIDI channel everything sends on
//   * "CC ASSIGNMENTS"      - which CC number each input emits
//   * "AccX MOD WHEEL"      - mod-wheel tilt range/deadzone
//   * "GYRO/ACCEL"          - amplification factor for gyro+accel CC output
//   * "MIDI NOTES"          - which note each D-pad / face button plays
//   * "MODIFIER DELTAS"     - how many semitones each modifier shifts by
//
// ============================================================================

#pragma once

#include <cstdint>

namespace mappings
{

// MIDI channel everything sends on. 0-indexed: 0 = MIDI channel 1, 15 = ch 16.
constexpr uint8_t MIDI_CHANNEL = 0;

// ----------------------------------------------------------------------------
// CC ASSIGNMENTS
// ----------------------------------------------------------------------------
// Sticks + trackpads land on Surge XT's default macro CCs (41-48) so macros
// 1-8 auto-route. Anything not in that range goes elsewhere; the "undefined"
// MIDI range 102-118 is used for Triton-specific inputs (capacitive touch,
// grip buttons, paddles, etc.) so they don't collide with anything standard.

// Trackpads + sticks (Surge XT macros 1-8). Trackpads on 1-4 because they
// retain their position when released, sticks on 5-8 since they spring back.
constexpr uint8_t CC_LPX = 41; // left pad X    -> macro 1
constexpr uint8_t CC_LPY = 42; // left pad Y    -> macro 2
constexpr uint8_t CC_RPX = 43; // right pad X   -> macro 3
constexpr uint8_t CC_RPY = 44; // right pad Y   -> macro 4
constexpr uint8_t CC_LSX = 45; // left stick X  -> macro 5
constexpr uint8_t CC_LSY = 46; // left stick Y  -> macro 6
constexpr uint8_t CC_RSX = 47; // right stick X -> macro 7
constexpr uint8_t CC_RSY = 48; // right stick Y -> macro 8

// Triggers, individual
constexpr uint8_t CC_LTRIGGER = 5;
constexpr uint8_t CC_RTRIGGER = 6;

// Trackpad pressures
constexpr uint8_t CC_LPAD_PRESSURE = 102;
constexpr uint8_t CC_RPAD_PRESSURE = 103;

// Face + shoulder + system buttons (digital, 127 on press / 0 on release)
constexpr uint8_t CC_A     = 20;
constexpr uint8_t CC_B     = 21;
constexpr uint8_t CC_X     = 22;
constexpr uint8_t CC_Y     = 23;
constexpr uint8_t CC_LB    = 24;
constexpr uint8_t CC_RB    = 25;
constexpr uint8_t CC_L3    = 26;  // left stick click
constexpr uint8_t CC_R3    = 27;  // right stick click
constexpr uint8_t CC_VIEW  = 28;  // Start (left of face buttons)
constexpr uint8_t CC_MENU  = 29;  // Select / Back (right of dpad)
constexpr uint8_t CC_DPAD_UP    = 30;
constexpr uint8_t CC_DPAD_DOWN  = 31;
constexpr uint8_t CC_DPAD_LEFT  = 32;
constexpr uint8_t CC_DPAD_RIGHT = 33;
constexpr uint8_t CC_STEAM = 34;  // guide button

// Gyro + accel (continuous)
constexpr uint8_t CC_GYRO_X  = 70;
constexpr uint8_t CC_GYRO_Y  = 71;
constexpr uint8_t CC_GYRO_Z  = 72;
constexpr uint8_t CC_ACCEL_X = 73;
constexpr uint8_t CC_ACCEL_Y = 74;
constexpr uint8_t CC_ACCEL_Z = 75;

// Triton-specific (Steam Controller's extra inputs)
constexpr uint8_t CC_LPAD_CLICK   = 104;
constexpr uint8_t CC_RPAD_CLICK   = 105;
constexpr uint8_t CC_LPAD_TOUCH   = 106; // capacitive sense, not click
constexpr uint8_t CC_RPAD_TOUCH   = 107;
constexpr uint8_t CC_LGRIP_TOUCH  = 108;
constexpr uint8_t CC_RGRIP_TOUCH  = 109;
constexpr uint8_t CC_LSTICK_TOUCH = 110;
constexpr uint8_t CC_RSTICK_TOUCH = 111;
constexpr uint8_t CC_LTRIG_CLICK  = 112; // trigger fully pulled
constexpr uint8_t CC_RTRIG_CLICK  = 113;
constexpr uint8_t CC_L4           = 114; // rear paddles
constexpr uint8_t CC_L5           = 115;
constexpr uint8_t CC_R4           = 116;
constexpr uint8_t CC_R5           = 117;
constexpr uint8_t CC_QAM          = 118; // quick-access menu

// ----------------------------------------------------------------------------
// AccX MOD WHEEL
// ----------------------------------------------------------------------------
// CC 1 (standard MIDI mod-wheel CC) is driven by the accelerometer's X-axis
// tilt. Set CC_MOD_WHEEL to 0 to disable.
constexpr uint8_t CC_MOD_WHEEL = 1;

// Raw |accel_x| below this threshold sends mod wheel = 0. Stops "controller
// sitting in your lap, slightly rotated" from spamming mod wheel events.
constexpr int ACCX_DEADZONE = 3000;

// Raw |accel_x| that maps to CC 127 (full mod wheel). ~16384 = 1g of tilt.
// Lower this for more sensitivity (less tilt to reach max mod wheel).
constexpr int ACCX_MAX = 13000;

// ----------------------------------------------------------------------------
// PITCH BEND
// ----------------------------------------------------------------------------
// LT pulls bend down, RT pulls bend up. At rest both triggers = 0, bend = 8192
// (center). Set to false to disable pitch bend entirely.
constexpr bool ENABLE_PITCH_BEND_FROM_TRIGGERS = true;

// ----------------------------------------------------------------------------
// GYRO / ACCEL OUTPUT BOOST
// ----------------------------------------------------------------------------
// Raw gyro/accel values rarely reach the full int16 range in practice.
// This multiplier is applied before scaling to CC, so normal motion can reach
// the full 0..127 CC range. 1 = no boost; 2 = double; etc.
constexpr int GYRO_ACCEL_BOOST = 2;

// ----------------------------------------------------------------------------
// NOTE LAYOUTS
// ----------------------------------------------------------------------------
// Two modes are available; switch between them at runtime by focusing the
// console window and pressing '1' (DIATONIC) or '2' (CHROMATIC).
//
//   DIATONIC  - Each button plays a fixed MIDI note (C major scale by default).
//               Modifiers (LB/RB/L4/R4/L5/R5) are MOMENTARY semitone shifts:
//               holding a modifier transposes any held notes; releasing it
//               transposes back. Releasing a face button while a modifier is
//               held sustains the note until all modifiers also release.
//
//   CHROMATIC - Each button plays an interval relative to a movable root note.
//               Modifiers TAP-SHIFT the root permanently (no momentary state):
//               press R4 once and the root moves up 1 semitone forever, until
//               you shift it back or press R3 to reset to C.

enum class NoteLayout
{
	DIATONIC,
	CHROMATIC,
};

constexpr NoteLayout DEFAULT_NOTE_LAYOUT = NoteLayout::DIATONIC;

// Velocity sent with every note_on (0-127). Applies to both layouts when
// ENABLE_ACCY_VELOCITY is false; otherwise this is the fallback ceiling.
constexpr int NOTE_VELOCITY = 100;

// AccY-driven velocity: tilt the controller "up" (positive AccY beyond the
// deadzone) to soften notes. Flat / tilted down = max velocity. Same deadzone
// shape as the AccX mod-wheel mapping.
constexpr bool ENABLE_ACCY_VELOCITY = true;
constexpr int ACCY_VELOCITY_DEADZONE = 3000;  // raw |AccY| below this = max velocity
constexpr int ACCY_VELOCITY_MAX_RAW = 12000;  // raw AccY where velocity bottoms out (lower = steeper)
constexpr int VELOCITY_MIN = 1;               // 0 would be a note-off, so floor at 1
constexpr int VELOCITY_MAX = 127;

// Set to false to disable note emission entirely (buttons still send their
// digital CCs from the CC ASSIGNMENTS section above).
constexpr bool ENABLE_NOTES = true;

// When true, trackpad CCs only update while the surface is being touched
// (capacitive sense). On release, the CC "freezes" at the last value.
// Sticks always send live values - they spring back to center physically.
constexpr bool FREEZE_TRACKPADS_ON_RELEASE = true;

// ----------------------------------------------------------------------------
// DIATONIC (Mode 1)
// ----------------------------------------------------------------------------
// MIDI note number each button plays. Default = C major diatonic from middle
// C (60). MIDI: 60=C, 61=C#, 62=D, 63=D#, 64=E, 65=F, 67=G, 69=A, 71=B, 72=C+1.

constexpr int NOTE_DPAD_DOWN  = 60; // C
constexpr int NOTE_DPAD_RIGHT = 62; // D
constexpr int NOTE_DPAD_LEFT  = 64; // E
constexpr int NOTE_DPAD_UP    = 65; // F
constexpr int NOTE_A          = 67; // G
constexpr int NOTE_B          = 69; // A
constexpr int NOTE_X          = 71; // B
constexpr int NOTE_Y          = 72; // C+1

// Momentary semitone shifts in DIATONIC mode. Sum of held modifiers = total
// offset added to the played note. E.g. LB + R4 held = -12 + 1 = -11.
constexpr int MOD_LB = -12; // octave down
constexpr int MOD_RB = +12; // octave up
constexpr int MOD_L4 = -1;  // semitone down
constexpr int MOD_R4 = +1;  // semitone up
constexpr int MOD_L5 = -2;  // whole tone down
constexpr int MOD_R5 = +2;  // whole tone up

// ----------------------------------------------------------------------------
// CHROMATIC (Mode 2)
// ----------------------------------------------------------------------------
// Step-instrument: every press plays (root + offset) AND advances root to that
// new note. A has offset 0 so pressing A plays the root without changing it.
// LB/RB silently nudge root by an octave. R3 resets root to default.

constexpr int CHROMATIC_DEFAULT_ROOT = 60;     // middle C (C4)
constexpr int CHROMATIC_DEFAULT_ALT_ROOT = 57; // A3 (below middle C), QAM toggles between C and this

// D-pad
constexpr int CHROMATIC_OFF_DPAD_UP    = -3; // minor 3rd down
constexpr int CHROMATIC_OFF_DPAD_DOWN  =  0; // root (same as A)
constexpr int CHROMATIC_OFF_DPAD_RIGHT = -1; // semitone down
constexpr int CHROMATIC_OFF_DPAD_LEFT  = -2; // whole step down

// Face buttons
constexpr int CHROMATIC_OFF_A          =  0; // root
constexpr int CHROMATIC_OFF_Y          = +3; // minor 3rd up
constexpr int CHROMATIC_OFF_X          = +1; // semitone up
constexpr int CHROMATIC_OFF_B          = +2; // whole step up

// Select / Start (small buttons either side of the controller body)
constexpr int CHROMATIC_OFF_VIEW       = +4; // Start (left of face buttons), major 3rd up
constexpr int CHROMATIC_OFF_MENU       = -4; // Select (right of dpad), major 3rd down

// Paddles (rear grip buttons)
constexpr int CHROMATIC_OFF_L4         = -6; // tritone down
constexpr int CHROMATIC_OFF_R4         = +6; // tritone up
constexpr int CHROMATIC_OFF_L5         = -7; // perfect 5th down
constexpr int CHROMATIC_OFF_R5         = +7; // perfect 5th up

// Shoulder bumpers - note-emitting in CHROMATIC mode.
constexpr int CHROMATIC_OFF_LB         = -5; // perfect 4th down
constexpr int CHROMATIC_OFF_RB         = +5; // perfect 4th up

} // namespace mappings
