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

// Sticks & trackpads (Surge XT macros 1-8)
constexpr uint8_t CC_LSX = 41; // left stick X  -> macro 1
constexpr uint8_t CC_LSY = 42; // left stick Y  -> macro 2
constexpr uint8_t CC_RSX = 43; // right stick X -> macro 3
constexpr uint8_t CC_RSY = 44; // right stick Y -> macro 4
constexpr uint8_t CC_LPX = 45; // left pad X    -> macro 5
constexpr uint8_t CC_LPY = 46; // left pad Y    -> macro 6
constexpr uint8_t CC_RPX = 47; // right pad X   -> macro 7
constexpr uint8_t CC_RPY = 48; // right pad Y   -> macro 8

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
constexpr uint8_t CC_VIEW  = 28;  // back / select
constexpr uint8_t CC_MENU  = 29;  // start
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
constexpr int ACCX_MAX = 16384;

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
// MIDI NOTES
// ----------------------------------------------------------------------------
// MIDI note number that each D-pad / face button plays. Defaults are a
// C-major diatonic scale starting at middle C (60). Change for other scales
// or layouts.
//
// MIDI numbers: 60 = middle C, 61 = C#, 62 = D, 63 = D#, 64 = E, 65 = F, ...
//               69 = A4 (440 Hz), 72 = C5, 84 = C6, etc.

constexpr int NOTE_DPAD_DOWN  = 60; // C
constexpr int NOTE_DPAD_RIGHT = 62; // D
constexpr int NOTE_DPAD_LEFT  = 64; // E
constexpr int NOTE_DPAD_UP    = 65; // F
constexpr int NOTE_A          = 67; // G
constexpr int NOTE_B          = 69; // A
constexpr int NOTE_X          = 71; // B
constexpr int NOTE_Y          = 72; // C+1

// Velocity sent with every note_on (0-127).
constexpr int NOTE_VELOCITY = 100;

// Set to false to disable note emission entirely (face buttons still send their
// CC events from the digital-button section above).
constexpr bool ENABLE_NOTES = true;

// ----------------------------------------------------------------------------
// MODIFIER DELTAS (semitones)
// ----------------------------------------------------------------------------
// Each modifier is a momentary hold. While held, it adds this many semitones
// to the currently-playing note(s). Sums combine, e.g. LB + R4 = -12 + 1 = -11.
// Each press/release retriggers held notes so you can play arpeggios.
//
// Releasing a face button while ANY of these is held keeps the note sustained
// until every modifier is also released (so you can play monophonic-synth
// melodies by tapping modifiers while a face button is held, then sustain).

constexpr int MOD_LB = -12; // octave down
constexpr int MOD_RB = +12; // octave up
constexpr int MOD_L4 = -1;  // semitone down
constexpr int MOD_R4 = +1;  // semitone up
constexpr int MOD_L5 = -2;  // whole tone down
constexpr int MOD_R5 = +2;  // whole tone up

} // namespace mappings
