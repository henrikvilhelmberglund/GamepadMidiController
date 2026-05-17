#include "GamepadMidiController.h"
#include "mappings.h"
#include "steam_controller.h"

#include <hidapi.h>
#include <Windows.h>
#include <conio.h>
#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

constexpr int BAR_WIDTH = 32;
constexpr int REDRAW_INTERVAL_MS = 33;

static void enable_vt_mode()
{
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD mode = 0;
	if (!GetConsoleMode(h, &mode)) return;
	SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static std::optional<libremidi::output_port> select_midi_out_port()
{
	libremidi::observer obs;
	std::cout << "Available MIDI output ports:\n";

	while (true)
	{
		auto ports = obs.get_output_ports();
		int i = 1;
		for (const libremidi::output_port& port : ports)
		{
			std::cout << i++ << " - " << port.port_name << '\n';
		}
		std::cout << "r - Refresh\n";
		std::cout << "0 - Exit\n";
		std::cout << "Select MIDI output port: ";

		std::string input;
		std::getline(std::cin, input);

		if (input == "r" || input == "R") continue;

		size_t option = 0;
		try { option = std::stoll(input); }
		catch (...) { continue; }

		if (option == 0) return std::nullopt;
		if (option > ports.size()) continue;
		return ports[option - 1];
	}
}

class MidiEmitter
{
public:
	MidiEmitter(libremidi::midi_out& midi, uint8_t channel) : m_midi(midi), m_channel(channel)
	{
		for (auto& v : m_last) v = -1;
	}

	void send(uint8_t cc, uint8_t value)
	{
		if (cc >= 128) return;
		if (m_last[cc] == static_cast<int16_t>(value)) return;
		m_last[cc] = value;
		m_midi.send_message(libremidi::channel_events::control_change(m_channel, cc, value));
		m_last_cc = cc;
		m_last_value = value;
	}

	void send_button(uint8_t cc, bool state) { send(cc, state ? 127 : 0); }

	void send_bipolar(uint8_t cc, int16_t value)
	{
		float normalized = (static_cast<float>(value) + 32768.0f) / 65535.0f;
		if (normalized < 0.0f) normalized = 0.0f;
		if (normalized > 1.0f) normalized = 1.0f;
		send(cc, static_cast<uint8_t>(std::round(normalized * 127.0f)));
	}

	void send_unipolar(uint8_t cc, int16_t value)
	{
		if (value < 0) value = 0;
		float normalized = static_cast<float>(value) / 32767.0f;
		if (normalized > 1.0f) normalized = 1.0f;
		send(cc, static_cast<uint8_t>(std::round(normalized * 127.0f)));
	}

	// Pitch bend: 14-bit value 0..16383, center 8192. Dedup so we don't spam.
	void send_pitch_bend(int value)
	{
		if (value < 0) value = 0;
		if (value > 16383) value = 16383;
		if (m_last_pitch_bend == value) return;
		m_last_pitch_bend = value;
		m_midi.send_message(libremidi::channel_events::pitch_bend(m_channel, value));
	}

	void send_note_on(uint8_t note, uint8_t vel)
	{
		m_midi.send_message(libremidi::channel_events::note_on(m_channel, note, vel));
	}
	void send_note_off(uint8_t note)
	{
		m_midi.send_message(libremidi::channel_events::note_off(m_channel, note, 0));
	}

	int last_cc() const { return m_last_cc; }
	int last_value() const { return m_last_value; }
	int value_for(uint8_t cc) const { return cc < 128 ? m_last[cc] : -1; }
	int last_pitch_bend() const { return m_last_pitch_bend; }

private:
	libremidi::midi_out& m_midi;
	uint8_t m_channel;
	int16_t m_last[128];
	int m_last_cc = -1;
	int m_last_value = -1;
	int m_last_pitch_bend = 8192;
};

// Drives MIDI note_on / note_off from the controller's button state.
//
// Two layouts, switchable at runtime by pressing '1' or '2' in the console:
//
//   DIATONIC (Mode 1) - fixed notes on D-pad + face buttons (C-major scale).
//     Modifiers (LB/RB/L4/R4/L5/R5) are momentary semitone shifts. Releasing
//     a face button while a modifier is held sustains the note.
//
//   CHROMATIC (Mode 2) - intervals around a movable root.
//     D-pad: +-1, +-2 (whole/half steps). Face: 0 / +-4 / +7. View/Menu: +-3.
//     Modifiers TAP-SHIFT the root permanently. R3 resets root to middle C.
class NoteEmitter
{
public:
	// 4 dpad + 4 face + view + menu + 4 paddles + 2 shoulders (LB/RB).
	// DIATONIC only uses indices 0..7. CHROMATIC uses all 16.
	static constexpr int NUM_BUTTONS = 16;

	NoteEmitter(MidiEmitter& m)
		: m_m(m),
		  m_layout(mappings::DEFAULT_NOTE_LAYOUT),
		  m_root(mappings::CHROMATIC_DEFAULT_ROOT)
	{
		for (auto& a : m_active) a = -1;
	}

	void set_layout(mappings::NoteLayout l)
	{
		if (l == m_layout) return;
		release_all();
		m_layout = l;
	}

	mappings::NoteLayout layout() const { return m_layout; }
	int total_offset() const { return m_offset; }
	int root() const { return m_root; }
	int home_root() const { return m_home_root; }
	int alt_root() const { return m_alt_root; }
	int mono_note() const { return m_mono_note; }
	int current_velocity() const { return m_current_velocity; }
	int active_note(int btn) const { return (btn >= 0 && btn < NUM_BUTTONS) ? m_active[btn] : -1; }

	void update(const sc::SCState& cur, const sc::SCState& prev)
	{
		m_current_velocity = compute_velocity(cur);
		bool now[NUM_BUTTONS] = {
			cur.dpad_down, cur.dpad_right, cur.dpad_left, cur.dpad_up,
			cur.a, cur.b, cur.x, cur.y,
			cur.view, cur.menu,
			cur.l4, cur.r4, cur.l5, cur.r5,
			cur.lb, cur.rb
		};
		bool was[NUM_BUTTONS] = {
			prev.dpad_down, prev.dpad_right, prev.dpad_left, prev.dpad_up,
			prev.a, prev.b, prev.x, prev.y,
			prev.view, prev.menu,
			prev.l4, prev.r4, prev.l5, prev.r5,
			prev.lb, prev.rb
		};

		if (m_layout == mappings::NoteLayout::DIATONIC)
			update_diatonic(cur, now, was);
		else
			update_chromatic(cur, prev, now, was);
	}

private:
	void release_all()
	{
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			if (m_active[i] >= 0)
			{
				m_m.send_note_off(static_cast<uint8_t>(m_active[i]));
				m_active[i] = -1;
			}
		}
		if (m_mono_note >= 0)
		{
			m_m.send_note_off(static_cast<uint8_t>(m_mono_note));
			m_mono_note = -1;
		}
		m_stack_count = 0;
	}

	// === DIATONIC: momentary modifiers + sustain ===
	void update_diatonic(const sc::SCState& cur, const bool now[], const bool was[])
	{
		int new_offset = compute_offset_diatonic(cur);
		bool offset_changed = (new_offset != m_offset);
		m_offset = new_offset;
		bool any_mod = any_modifier_held(cur);

		// Release notes whose face button is released AND no modifier sustaining.
		for (int i = 0; i < 8; ++i)
		{
			if (m_active[i] >= 0 && !now[i] && !any_mod)
			{
				m_m.send_note_off(static_cast<uint8_t>(m_active[i]));
				m_active[i] = -1;
			}
		}

		// Offset change retriggers held notes (arpeggio effect).
		if (offset_changed)
		{
			for (int i = 0; i < 8; ++i)
			{
				if (m_active[i] >= 0)
				{
					m_m.send_note_off(static_cast<uint8_t>(m_active[i]));
					int n = effective_note_diatonic(i);
					m_active[i] = (n >= 0 && n <= 127) ? n : -1;
					if (m_active[i] >= 0)
						m_m.send_note_on(static_cast<uint8_t>(m_active[i]), static_cast<uint8_t>(m_current_velocity));
				}
			}
		}

		// New face-button presses.
		for (int i = 0; i < 8; ++i)
		{
			if (now[i] && !was[i])
			{
				if (m_active[i] >= 0)
					m_m.send_note_off(static_cast<uint8_t>(m_active[i]));
				int n = effective_note_diatonic(i);
				if (n >= 0 && n <= 127)
				{
					m_active[i] = n;
					m_m.send_note_on(static_cast<uint8_t>(n), static_cast<uint8_t>(m_current_velocity));
				}
			}
		}
	}

	// === CHROMATIC: monophonic press-stack ===
	// Only the FIRST press (when nothing else is held) advances the root.
	// Subsequent presses while a button is held just shift the active note.
	// On release, the note reverts to whichever button is now top of the stack.
	void update_chromatic(const sc::SCState& cur, const sc::SCState& prev,
		const bool now[], const bool was[])
	{
		// Silent control buttons (don't enter the press stack, don't play notes).
		// R3/L3 snap to the "home" root (which QAM and Steam update).
		if (cur.r3 && !prev.r3) m_root = m_home_root;
		if (cur.l3 && !prev.l3) m_root = m_home_root;
		if (cur.qam && !prev.qam)
		{
			m_at_alt = !m_at_alt;
			m_root = m_at_alt ? m_alt_root : mappings::CHROMATIC_DEFAULT_ROOT;
			m_home_root = m_root;
		}
		if (cur.steam && !prev.steam)
		{
			m_alt_root = m_root;
			m_home_root = m_root;
		}
		if (m_root < 0)   m_root = 0;
		if (m_root > 127) m_root = 127;
		if (m_alt_root < 0)   m_alt_root = 0;
		if (m_alt_root > 127) m_alt_root = 127;
		if (m_home_root < 0)   m_home_root = 0;
		if (m_home_root > 127) m_home_root = 127;

		// Process releases first (remove from stack).
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			if (!now[i] && was[i]) stack_remove(i);
		}

		// Process presses: push to stack, advance root if this is the primary
		// press (stack was empty before this push).
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			if (now[i] && !was[i])
			{
				bool primary = (m_stack_count == 0);
				if (primary)
				{
					int n = clamp_midi(m_root + chromatic_offset(i));
					m_root = n;
				}
				stack_push(i, primary);
			}
		}

		// Sync the single playing note to whatever sits on top of the stack.
		// Primary's offset is already absorbed into m_root; secondaries get
		// their offset added on top.
		int desired = -1;
		if (m_stack_count > 0)
		{
			const PressInfo& top = m_press_stack[m_stack_count - 1];
			desired = top.primary
				? clamp_midi(m_root)
				: clamp_midi(m_root + chromatic_offset(top.button));
		}
		else if (m_mono_note >= 0)
		{
			// Last finger just lifted - advance root to the final note that
			// was playing, so the next press starts relative to it.
			m_root = m_mono_note;
		}
		if (desired != m_mono_note)
		{
			if (m_mono_note >= 0) m_m.send_note_off(static_cast<uint8_t>(m_mono_note));
			if (desired >= 0) m_m.send_note_on(static_cast<uint8_t>(desired), static_cast<uint8_t>(m_current_velocity));
			m_mono_note = desired;
		}
	}

	static int clamp_midi(int n)
	{
		if (n < 0) return 0;
		if (n > 127) return 127;
		return n;
	}

	void stack_push(int btn, bool primary)
	{
		if (m_stack_count < NUM_BUTTONS) m_press_stack[m_stack_count++] = { btn, primary };
	}

	void stack_remove(int btn)
	{
		for (int i = 0; i < m_stack_count; ++i)
		{
			if (m_press_stack[i].button == btn)
			{
				for (int j = i; j < m_stack_count - 1; ++j)
					m_press_stack[j] = m_press_stack[j + 1];
				--m_stack_count;
				return;
			}
		}
	}

	static int chromatic_offset(int button)
	{
		static constexpr int OFFSET[NUM_BUTTONS] = {
			mappings::CHROMATIC_OFF_DPAD_DOWN, mappings::CHROMATIC_OFF_DPAD_RIGHT,
			mappings::CHROMATIC_OFF_DPAD_LEFT, mappings::CHROMATIC_OFF_DPAD_UP,
			mappings::CHROMATIC_OFF_A, mappings::CHROMATIC_OFF_B,
			mappings::CHROMATIC_OFF_X, mappings::CHROMATIC_OFF_Y,
			mappings::CHROMATIC_OFF_VIEW, mappings::CHROMATIC_OFF_MENU,
			mappings::CHROMATIC_OFF_L4, mappings::CHROMATIC_OFF_R4,
			mappings::CHROMATIC_OFF_L5, mappings::CHROMATIC_OFF_R5,
			mappings::CHROMATIC_OFF_LB, mappings::CHROMATIC_OFF_RB,
		};
		return OFFSET[button];
	}

	static int compute_offset_diatonic(const sc::SCState& s)
	{
		int offset = 0;
		if (s.lb) offset += mappings::MOD_LB;
		if (s.rb) offset += mappings::MOD_RB;
		if (s.l4) offset += mappings::MOD_L4;
		if (s.r4) offset += mappings::MOD_R4;
		if (s.l5) offset += mappings::MOD_L5;
		if (s.r5) offset += mappings::MOD_R5;
		return offset;
	}

	static bool any_modifier_held(const sc::SCState& s)
	{
		return s.lb || s.rb || s.l4 || s.r4 || s.l5 || s.r5;
	}

	// AccY tilt drives note velocity. Flat / tilted down = max velocity, tilted
	// up (positive AccY beyond deadzone) reduces velocity proportionally.
	static int compute_velocity(const sc::SCState& s)
	{
		if constexpr (!mappings::ENABLE_ACCY_VELOCITY)
		{
			return mappings::NOTE_VELOCITY;
		}
		int ay = s.accel_y;
		if (ay < mappings::ACCY_VELOCITY_DEADZONE)
			return mappings::VELOCITY_MAX;
		float t = static_cast<float>(ay - mappings::ACCY_VELOCITY_DEADZONE)
			/ static_cast<float>(mappings::ACCY_VELOCITY_MAX_RAW - mappings::ACCY_VELOCITY_DEADZONE);
		if (t > 1.0f) t = 1.0f;
		int span = mappings::VELOCITY_MAX - mappings::VELOCITY_MIN;
		int v = mappings::VELOCITY_MAX - static_cast<int>(std::round(t * span));
		if (v < mappings::VELOCITY_MIN) v = mappings::VELOCITY_MIN;
		if (v > mappings::VELOCITY_MAX) v = mappings::VELOCITY_MAX;
		return v;
	}

	int effective_note_diatonic(int button) const
	{
		static constexpr int BASE[8] = {
			mappings::NOTE_DPAD_DOWN, mappings::NOTE_DPAD_RIGHT,
			mappings::NOTE_DPAD_LEFT, mappings::NOTE_DPAD_UP,
			mappings::NOTE_A, mappings::NOTE_B,
			mappings::NOTE_X, mappings::NOTE_Y,
		};
		return BASE[button] + m_offset;
	}

	MidiEmitter& m_m;
	mappings::NoteLayout m_layout;
	int m_offset = 0;            // DIATONIC: momentary modifier sum
	int m_root;                  // CHROMATIC: persistent root (drifts with each press)
	int m_home_root = mappings::CHROMATIC_DEFAULT_ROOT; // L3/R3 snap target
	int m_alt_root = mappings::CHROMATIC_DEFAULT_ALT_ROOT;
	bool m_at_alt = false;       // CHROMATIC: which side of the QAM toggle we're on
	int m_active[NUM_BUTTONS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
	int m_mono_note = -1;        // CHROMATIC: the single currently-playing note
	int m_current_velocity = mappings::NOTE_VELOCITY; // recomputed each frame from AccY
	struct PressInfo { int button; bool primary; };
	PressInfo m_press_stack[NUM_BUTTONS] = {};
	int m_stack_count = 0;
};

static void emit_state(MidiEmitter& m, const sc::SCState& s)
{
	using namespace mappings;

	// Sticks always send live values (they spring back to center). Trackpads
	// gate on capacitive touch so the CC freezes at the last value on release.
	m.send_bipolar(CC_LSX, s.lstick_x);
	m.send_bipolar(CC_LSY, s.lstick_y);
	m.send_bipolar(CC_RSX, s.rstick_x);
	m.send_bipolar(CC_RSY, s.rstick_y);
	if (!FREEZE_TRACKPADS_ON_RELEASE || s.lpad_touch)
	{
		m.send_bipolar(CC_LPX, s.lpad_x);
		m.send_bipolar(CC_LPY, s.lpad_y);
	}
	if (!FREEZE_TRACKPADS_ON_RELEASE || s.rpad_touch)
	{
		m.send_bipolar(CC_RPX, s.rpad_x);
		m.send_bipolar(CC_RPY, s.rpad_y);
	}

	// Triggers
	m.send_unipolar(CC_LTRIGGER, s.ltrigger);
	m.send_unipolar(CC_RTRIGGER, s.rtrigger);

	// Pitch bend: RT bends up, LT bends down, center 8192 at rest.
	if constexpr (ENABLE_PITCH_BEND_FROM_TRIGGERS)
	{
		float lt = std::max(0, static_cast<int>(s.ltrigger)) / 32767.0f;
		float rt = std::max(0, static_cast<int>(s.rtrigger)) / 32767.0f;
		float v = 0.5f + (rt - lt) * 0.5f;
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		m.send_pitch_bend(static_cast<int>(std::round(v * 16383.0f)));
	}

	// AccX tilt -> mod wheel (CC 1) with deadzone
	if constexpr (CC_MOD_WHEEL != 0)
	{
		int ax = s.accel_x;
		int magnitude = ax < 0 ? -ax : ax;
		uint8_t mw = 0;
		if (magnitude > ACCX_DEADZONE)
		{
			float scaled = static_cast<float>(magnitude - ACCX_DEADZONE)
				/ static_cast<float>(ACCX_MAX - ACCX_DEADZONE);
			if (scaled < 0.0f) scaled = 0.0f;
			if (scaled > 1.0f) scaled = 1.0f;
			mw = static_cast<uint8_t>(std::round(scaled * 127.0f));
		}
		m.send(CC_MOD_WHEEL, mw);
	}

	// Face / system buttons
	m.send_button(CC_A, s.a);
	m.send_button(CC_B, s.b);
	m.send_button(CC_X, s.x);
	m.send_button(CC_Y, s.y);
	m.send_button(CC_LB, s.lb);
	m.send_button(CC_RB, s.rb);
	m.send_button(CC_L3, s.l3);
	m.send_button(CC_R3, s.r3);
	m.send_button(CC_VIEW, s.view);
	m.send_button(CC_MENU, s.menu);
	m.send_button(CC_DPAD_UP, s.dpad_up);
	m.send_button(CC_DPAD_DOWN, s.dpad_down);
	m.send_button(CC_DPAD_LEFT, s.dpad_left);
	m.send_button(CC_DPAD_RIGHT, s.dpad_right);
	m.send_button(CC_STEAM, s.steam);

	// Gyro + accel, boosted before scaling so normal motion reaches full CC range.
	auto boost = [](int16_t v) -> int16_t {
		int b = static_cast<int>(v) * GYRO_ACCEL_BOOST;
		if (b > 32767) return 32767;
		if (b < -32768) return -32768;
		return static_cast<int16_t>(b);
	};
	m.send_bipolar(CC_GYRO_X, boost(s.gyro_x));
	m.send_bipolar(CC_GYRO_Y, boost(s.gyro_y));
	m.send_bipolar(CC_GYRO_Z, boost(s.gyro_z));
	m.send_bipolar(CC_ACCEL_X, boost(s.accel_x));
	m.send_bipolar(CC_ACCEL_Y, boost(s.accel_y));
	m.send_bipolar(CC_ACCEL_Z, boost(s.accel_z));

	// Triton-specific clicks/touches/paddles/QAM
	if (!FREEZE_TRACKPADS_ON_RELEASE || s.lpad_touch)
		m.send_unipolar(CC_LPAD_PRESSURE, s.lpad_pressure);
	if (!FREEZE_TRACKPADS_ON_RELEASE || s.rpad_touch)
		m.send_unipolar(CC_RPAD_PRESSURE, s.rpad_pressure);
	m.send_button(CC_LPAD_CLICK, s.lpad_click);
	m.send_button(CC_RPAD_CLICK, s.rpad_click);
	m.send_button(CC_LPAD_TOUCH, s.lpad_touch);
	m.send_button(CC_RPAD_TOUCH, s.rpad_touch);
	m.send_button(CC_LGRIP_TOUCH, s.lgrip_touch);
	m.send_button(CC_RGRIP_TOUCH, s.rgrip_touch);
	m.send_button(CC_LSTICK_TOUCH, s.lstick_touch);
	m.send_button(CC_RSTICK_TOUCH, s.rstick_touch);
	m.send_button(CC_LTRIG_CLICK, s.ltrig_click);
	m.send_button(CC_RTRIG_CLICK, s.rtrig_click);
	m.send_button(CC_L4, s.l4);
	m.send_button(CC_L5, s.l5);
	m.send_button(CC_R4, s.r4);
	m.send_button(CC_R5, s.r5);
	m.send_button(CC_QAM, s.qam);
}

static int bar_index(int raw, int range, bool bipolar)
{
	double normalized = bipolar
		? (static_cast<double>(raw) + range) / (2.0 * range)
		: static_cast<double>(raw) / range;
	if (normalized < 0.0) normalized = 0.0;
	if (normalized > 1.0) normalized = 1.0;
	return static_cast<int>(normalized * BAR_WIDTH);
}

static std::string make_bar(int raw, int range, bool bipolar)
{
	int filled = bar_index(raw, range, bipolar);
	int center = BAR_WIDTH / 2;
	std::string s(BAR_WIDTH, '.');
	for (int i = 0; i < filled && i < BAR_WIDTH; i++) s[i] = '#';
	if (bipolar && center < BAR_WIDTH && s[center] == '.') s[center] = '|';
	return "[" + s + "]";
}

static const char* on_off(bool b) { return b ? "ON " : "   "; }

static void render(std::ostringstream& os, const char* label, int raw, int range, bool bipolar,
	uint8_t cc, const MidiEmitter& m)
{
	int cc_val = m.value_for(cc);
	os << "  " << std::left << std::setw(5) << label << std::right
		<< make_bar(raw, range, bipolar)
		<< " " << std::setw(7) << raw
		<< "  CC" << std::setw(3) << static_cast<int>(cc)
		<< "=" << std::setw(3) << (cc_val < 0 ? 0 : cc_val)
		<< "\033[K\n";
}

static void redraw(const sc::SCState& s, const MidiEmitter& m, const NoteEmitter& n)
{
	std::ostringstream os;
	static bool first = true;
	if (first)
	{
		// One-time setup: clear screen, hide caret. Subsequent frames overwrite
		// content in place using \033[K per row instead of clearing the screen,
		// which avoids the brief "black flash" between clear and re-draw.
		os << "\033[H\033[2J\033[?25l";
		first = false;
	}
	else
	{
		os << "\033[H";
	}

	render(os, "LSX",  s.lstick_x, 32767, true, mappings::CC_LSX, m);
	render(os, "LSY",  s.lstick_y, 32767, true, mappings::CC_LSY, m);
	render(os, "RSX",  s.rstick_x, 32767, true, mappings::CC_RSX, m);
	render(os, "RSY",  s.rstick_y, 32767, true, mappings::CC_RSY, m);
	render(os, "LT",   s.ltrigger, 32767, false, mappings::CC_LTRIGGER, m);
	render(os, "RT",   s.rtrigger, 32767, false, mappings::CC_RTRIGGER, m);
	{
		int pb = m.last_pitch_bend();
		os << "  " << std::left << std::setw(5) << "Pitch" << std::right
			<< make_bar(pb - 8192, 8191, true)
			<< " " << std::setw(7) << pb
			<< "  PITCH BEND\033[K\n";
	}
	render(os, "LPX",  s.lpad_x, 32767, true, mappings::CC_LPX, m);
	render(os, "LPY",  s.lpad_y, 32767, true, mappings::CC_LPY, m);
	render(os, "RPX",  s.rpad_x, 32767, true, mappings::CC_RPX, m);
	render(os, "RPY",  s.rpad_y, 32767, true, mappings::CC_RPY, m);
	os << "  LPP=" << std::setw(3) << static_cast<int>(m.value_for(mappings::CC_LPAD_PRESSURE) < 0 ? 0 : m.value_for(mappings::CC_LPAD_PRESSURE))
		<< "  RPP=" << std::setw(3) << static_cast<int>(m.value_for(mappings::CC_RPAD_PRESSURE) < 0 ? 0 : m.value_for(mappings::CC_RPAD_PRESSURE))
		<< "    MW=" << std::setw(3) << static_cast<int>(m.value_for(mappings::CC_MOD_WHEEL) < 0 ? 0 : m.value_for(mappings::CC_MOD_WHEEL))
		<< " (CC " << static_cast<int>(mappings::CC_MOD_WHEEL) << ", from AccX tilt)\033[K\n";
	os << "  Gyro:  X" << std::showpos << std::setw(7) << s.gyro_x
		<< "  Y" << std::setw(7) << s.gyro_y
		<< "  Z" << std::setw(7) << s.gyro_z << std::noshowpos
		<< "   CC " << static_cast<int>(mappings::CC_GYRO_X)
		<< "/" << static_cast<int>(mappings::CC_GYRO_Y)
		<< "/" << static_cast<int>(mappings::CC_GYRO_Z) << "\033[K\n";
	os << "  Accel: X" << std::showpos << std::setw(7) << s.accel_x
		<< "  Y" << std::setw(7) << s.accel_y
		<< "  Z" << std::setw(7) << s.accel_z << std::noshowpos
		<< "   CC " << static_cast<int>(mappings::CC_ACCEL_X)
		<< "/" << static_cast<int>(mappings::CC_ACCEL_Y)
		<< "/" << static_cast<int>(mappings::CC_ACCEL_Z) << "\033[K\n";

	os << "  Buttons:  "
		<< "A:" << on_off(s.a) << "B:" << on_off(s.b) << "X:" << on_off(s.x) << "Y:" << on_off(s.y)
		<< " LB:" << on_off(s.lb) << "RB:" << on_off(s.rb)
		<< " L3:" << on_off(s.l3) << "R3:" << on_off(s.r3) << "\033[K\n";
	os << "  Sys:      "
		<< "VIEW:" << on_off(s.view) << "MENU:" << on_off(s.menu)
		<< " STEAM:" << on_off(s.steam) << "QAM:" << on_off(s.qam)
		<< "  DPad U:" << on_off(s.dpad_up) << "D:" << on_off(s.dpad_down)
		<< "L:" << on_off(s.dpad_left) << "R:" << on_off(s.dpad_right) << "\033[K\n";
	os << "  Touch:    "
		<< "LPad T:" << on_off(s.lpad_touch) << "C:" << on_off(s.lpad_click)
		<< " RPad T:" << on_off(s.rpad_touch) << "C:" << on_off(s.rpad_click)
		<< " LGrip:" << on_off(s.lgrip_touch) << " RGrip:" << on_off(s.rgrip_touch) << "\033[K\n";
	os << "  Pull/Pad: "
		<< "LTrigC:" << on_off(s.ltrig_click) << "RTrigC:" << on_off(s.rtrig_click)
		<< " L4:" << on_off(s.l4) << "L5:" << on_off(s.l5)
		<< " R4:" << on_off(s.r4) << "R5:" << on_off(s.r5) << "\033[K\n";
	{
		bool diatonic = (n.layout() == mappings::NoteLayout::DIATONIC);
		os << "  Notes:    mode=" << (diatonic ? "DIATONIC " : "CHROMATIC")
			<< " (press 1 or 2)  ";
		if (diatonic)
		{
			os << "offset=" << std::showpos << n.total_offset() << std::noshowpos << " semis";
			os << "  active=[";
			bool ffirst = true;
			for (int i = 0; i < NoteEmitter::NUM_BUTTONS; ++i)
			{
				int an = n.active_note(i);
				if (an >= 0)
				{
					if (!ffirst) os << " ";
					os << an;
					ffirst = false;
				}
			}
			os << "]";
		}
		else
		{
			os << "root=" << n.root() << " home=" << n.home_root()
				<< " alt=" << n.alt_root() << " vel=" << n.current_velocity()
				<< " playing=";
			if (n.mono_note() >= 0) os << n.mono_note();
			else os << "-";
		}
		os << "\033[K\n";
	}
	os << "  Last MIDI: ";
	if (m.last_cc() >= 0)
		os << "CC " << std::setw(3) << m.last_cc() << " = " << std::setw(3) << m.last_value();
	else
		os << "(none yet)";
	os << "\033[K\n";

	std::cout << os.str();
	std::cout.flush();
}

int main()
{
	enable_vt_mode();

	std::cout << "GamepadMidiController - Steam Controller Triton + HIDAPI + MIDI\n\n";

	auto out_port = select_midi_out_port();
	if (!out_port)
	{
		std::cout << "No MIDI output port selected. Exiting.\n";
		return 0;
	}

	std::cout << "\nSelected MIDI port: " << out_port->port_name << '\n';
	libremidi::midi_out midi;
	midi.open_port(*out_port);

	if (hid_init() != 0)
	{
		std::cerr << "hid_init() failed\n";
		return 1;
	}

	sc::SteamController controller;
	if (!controller.open_first())
	{
		std::cerr << "\nNo Steam Controller found. Make sure Steam is closed and the controller is on.\n";
		hid_exit();
		return 1;
	}

	const char* kind = "unknown";
	switch (controller.product_id())
	{
	case sc::SC_PID_WIRED:  kind = "wired"; break;
	case sc::SC_PID_DONGLE: kind = "dongle"; break;
	case sc::SC_PID_PUCK:   kind = "puck (Triton wireless receiver)"; break;
	}
	std::cout << "Opened SC: PID=0x" << std::hex << controller.product_id() << std::dec
		<< " (" << kind << ")\n";

	if (!controller.configure_for_raw_input())
	{
		std::cerr << "Failed to disable lizard mode / enable IMU\n";
		hid_exit();
		return 1;
	}

	std::cout << "Lizard mode disabled + IMU streaming enabled.\n";
	std::cout << "Sending MIDI CC on channel " << static_cast<int>(mappings::MIDI_CHANNEL + 1)
		<< ". Live dashboard (Ctrl+C to exit):\n\n";

	MidiEmitter emitter(midi, mappings::MIDI_CHANNEL);
	NoteEmitter notes(emitter);

	auto last_lizard_refresh = std::chrono::steady_clock::now();
	auto last_redraw = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	sc::SCState state;
	sc::SCState prev_state;
	bool have_prev = false;
	while (true)
	{
		auto now = std::chrono::steady_clock::now();
		if (now - last_lizard_refresh >= std::chrono::seconds(3))
		{
			controller.keep_lizard_mode_disabled();
			last_lizard_refresh = now;
		}

		auto r = controller.read(100);
		if (!r) continue;

		if (!sc::parse_triton_report(r->bytes.data(), r->bytes.size(), state))
		{
			continue;
		}

		// Keyboard mode switching: focus the console and press 1 or 2.
		while (_kbhit())
		{
			int c = _getch();
			if (c == '1') notes.set_layout(mappings::NoteLayout::DIATONIC);
			else if (c == '2') notes.set_layout(mappings::NoteLayout::CHROMATIC);
		}

		emit_state(emitter, state);
		if constexpr (mappings::ENABLE_NOTES)
		{
			if (have_prev) notes.update(state, prev_state);
		}
		prev_state = state;
		have_prev = true;

		if (now - last_redraw >= std::chrono::milliseconds(REDRAW_INTERVAL_MS))
		{
			redraw(state, emitter, notes);
			last_redraw = now;
		}
	}

	midi.close_port();
	hid_exit();
	return 0;
}
