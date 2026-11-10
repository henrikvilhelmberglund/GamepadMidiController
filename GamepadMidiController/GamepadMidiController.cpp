#include "GamepadMidiController.h"
#include "mappings.h"
#include "steam_controller.h"

#include <hidapi.h>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#include <TlHelp32.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#endif

namespace platform
{
#ifdef _WIN32

	inline void init()
	{
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		if (h == INVALID_HANDLE_VALUE) return;
		DWORD mode = 0;
		if (!GetConsoleMode(h, &mode)) return;
		SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
	inline void cleanup()
	{
		// Show cursor (we hide it during dashboard rendering).
		std::cout << "\033[?25h" << std::flush;
	}
	inline bool poll_key(int& out_key)
	{
		if (_kbhit()) { out_key = _getch(); return true; }
		return false;
	}

	// Steam grabs the controller's HID device exclusively, so we can't use it
	// while steam.exe is alive.
	inline bool is_steam_running()
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) return false;
		PROCESSENTRY32W pe{};
		pe.dwSize = sizeof(pe);
		bool found = false;
		if (Process32FirstW(snap, &pe))
		{
			do
			{
				if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) { found = true; break; }
			} while (Process32NextW(snap, &pe));
		}
		CloseHandle(snap);
		return found;
	}

	// When launched by double-clicking the exe, the console window belongs to
	// us alone and vanishes on exit - so wait for a key before closing it, or
	// the error message is unreadable.
	inline void pause_before_exit()
	{
		DWORD ids[2];
		if (GetConsoleProcessList(ids, 2) <= 1)
		{
			std::cout << "\nPress any key to exit..." << std::flush;
			(void)_getch();
		}
	}

#else // POSIX: raw stdin via termios + non-blocking poll via select()

	inline ::termios& saved_tios()
	{
		static ::termios t{};
		return t;
	}
	inline bool& tios_saved()
	{
		static bool b = false;
		return b;
	}

	inline void init()
	{
		::termios t{};
		if (tcgetattr(STDIN_FILENO, &t) == 0)
		{
			saved_tios() = t;
			tios_saved() = true;
			t.c_lflag &= ~(ICANON | ECHO); // raw, no echo
			tcsetattr(STDIN_FILENO, TCSANOW, &t);
		}
	}
	inline void cleanup()
	{
		if (tios_saved())
			tcsetattr(STDIN_FILENO, TCSANOW, &saved_tios());
		const char show_cursor[] = "\033[?25h";
		(void)write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
	}
	inline bool poll_key(int& out_key)
	{
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		::timeval tv{};
		if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0)
		{
			unsigned char c = 0;
			if (read(STDIN_FILENO, &c, 1) == 1) { out_key = c; return true; }
		}
		return false;
	}

	inline bool is_steam_running()
	{
#ifdef __linux__
		try
		{
			namespace fs = std::filesystem;
			for (const auto& e : fs::directory_iterator("/proc"))
			{
				const std::string pid = e.path().filename().string();
				if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0]))) continue;
				std::ifstream f(e.path() / "comm");
				std::string comm;
				if (std::getline(f, comm) && comm == "steam") return true;
			}
		}
		catch (...) {}
		return false;
#else
		return std::system("pgrep -xq steam_osx") == 0;
#endif
	}

	inline void pause_before_exit() {} // terminal stays open on POSIX

#endif

	extern "C" inline void on_signal(int)
	{
		cleanup();
		_Exit(0);
	}

	inline void install_signal_handler()
	{
		std::signal(SIGINT, on_signal);
		std::signal(SIGTERM, on_signal);
	}

} // namespace platform

constexpr int BAR_WIDTH = 32;
constexpr int REDRAW_INTERVAL_MS = 33;

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

static bool icontains(const std::string& haystack, const std::string& needle)
{
	auto lower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(),
			[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return s;
	};
	return lower(haystack).find(lower(needle)) != std::string::npos;
}

// Block until Steam is closed and a controller answers on the receiver,
// showing a one-line status meanwhile. Returns false if the user quits (Q/Esc).
static bool wait_for_controller(sc::SteamController& controller)
{
	static constexpr char SPINNER[] = "|/-\\";
	int spin = 0;
	bool waited = false;
	while (true)
	{
		bool steam = platform::is_steam_running();
		if (!steam && controller.open_first(false))
		{
			if (waited) std::cout << "\r\033[K";
			return true;
		}
		waited = true;

		std::cout << "\r\033[K" << SPINNER[spin++ % 4] << ' '
			<< (steam
				? "Steam is running. Quit it (tray icon > Exit) to free the controller."
				: "Waiting for controller. Turn it on or press a button to wake it.")
			<< "  Q = quit" << std::flush;

		for (int i = 0; i < 10; ++i) // ~500 ms between attempts, keys stay responsive
		{
			int c;
			while (platform::poll_key(c))
			{
				if (c == 'q' || c == 'Q' || c == 27)
				{
					std::cout << '\n';
					return false;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}
}

class MidiEmitter
{
public:
	// `channel` is 0-indexed (0 = MIDI channel 1), like everything in
	// mappings.h. libremidi's channel_events are 1-indexed, hence the +1.
	MidiEmitter(libremidi::midi_out& midi, uint8_t channel)
		: m_midi(midi), m_channel(static_cast<uint8_t>(channel + 1))
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
		send_continuous(cc, normalized * 127.0f);
	}

	void send_unipolar(uint8_t cc, int16_t value)
	{
		if (value < 0) value = 0;
		float normalized = static_cast<float>(value) / 32767.0f;
		send_continuous(cc, normalized * 127.0f);
	}

	// `v` is the unrounded 0..127 value. Hysteresis: only move to a new CC
	// value once the input is clearly past the rounding midpoint. Without it,
	// a resting sensor sitting on a boundary (gyro/accel/sticks at rest map to
	// exactly 63.5) flickers 63/64 forever, flooding the DAW and breaking
	// MIDI learn.
	void send_continuous(uint8_t cc, float v)
	{
		if (cc >= 128) return;
		if (v < 0.0f) v = 0.0f;
		if (v > 127.0f) v = 127.0f;
		int last = m_last[cc];
		if (last >= 0 && std::fabs(v - static_cast<float>(last)) < CONTINUOUS_HYSTERESIS) return;
		send(cc, static_cast<uint8_t>(std::round(v)));
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
	static constexpr float CONTINUOUS_HYSTERESIS = 0.75f; // in CC steps (0.5 = none)

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

	// Note-off for everything currently sounding (e.g. when entering DAW mode).
	void panic() { release_all(); }
	int total_offset() const { return m_offset; }
	int root() const { return m_root; }
	int home_root() const { return m_home_root; }
	int alt_root() const { return m_alt_root; }
	int mono_note() const { return m_mono_note; }
	int current_velocity() const { return m_current_velocity; }

	// SCALE/CHORD mode getters
	int scale_tonic() const { return m_tonic; }
	int scale_alt_tonic() const { return m_alt_tonic; }
	int scale_index() const { return m_scale_index; }
	const char* scale_name() const { return mappings::SCALES[m_scale_index].name; }
	bool chord_root_mode() const { return m_chord_root_mode; }
	int lsdj_root() const { return m_lsdj_root; }
	int lsdj_note() const { return m_lsdj_note; }
	bool lsdj_qam_mode() const { return m_lsdj_qam_mode; }
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
		else if (m_layout == mappings::NoteLayout::CHROMATIC)
			update_chromatic(cur, prev, now, was);
		else if (m_layout == mappings::NoteLayout::SCALE)
			update_scale(cur, prev, now, was);
		else if (m_layout == mappings::NoteLayout::CHORD)
			update_chord(cur, prev);
		else
			update_lsdj(cur, prev);
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
		// CHORD voices
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			for (int v = 0; v < m_chord_voice_count[i]; ++v)
			{
				if (m_chord_notes[i][v] >= 0)
					m_m.send_note_off(static_cast<uint8_t>(m_chord_notes[i][v]));
				m_chord_notes[i][v] = -1;
			}
			m_chord_voice_count[i] = 0;
		}
		// LSDJ note
		if (m_lsdj_note >= 0)
		{
			m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
			m_lsdj_note = -1;
		}
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

	// === SCALE: polyphonic, fixed (no root drift). Each button plays
	// (tonic + scale-step offset) in the currently-selected scale.
	// L3/R3 cycle scales, QAM toggles tonic, Steam saves alt tonic.
	void update_scale(const sc::SCState& cur, const sc::SCState& prev,
		const bool /*now*/[], const bool /*was*/[])
	{
		// Scale changes
		if (cur.l3 && !prev.l3)
			m_scale_index = (m_scale_index + mappings::SCALE_COUNT - 1) % mappings::SCALE_COUNT;
		if (cur.r3 && !prev.r3)
			m_scale_index = (m_scale_index + 1) % mappings::SCALE_COUNT;

		// Tonic toggle + save
		if (cur.qam && !prev.qam)
		{
			m_at_alt = !m_at_alt;
			m_tonic = m_at_alt ? m_alt_tonic : mappings::SCALE_DEFAULT_TONIC;
		}
		if (cur.steam && !prev.steam)
		{
			m_alt_tonic = m_tonic;
		}

		bool buttons_now[NUM_BUTTONS] = {
			cur.dpad_down, cur.dpad_right, cur.dpad_left, cur.dpad_up,
			cur.a, cur.b, cur.x, cur.y,
			cur.view, cur.menu,
			cur.l4, cur.r4, cur.l5, cur.r5,
			cur.lb, cur.rb
		};
		bool buttons_was[NUM_BUTTONS] = {
			prev.dpad_down, prev.dpad_right, prev.dpad_left, prev.dpad_up,
			prev.a, prev.b, prev.x, prev.y,
			prev.view, prev.menu,
			prev.l4, prev.r4, prev.l5, prev.r5,
			prev.lb, prev.rb
		};

		// Releases: stop notes for buttons no longer held.
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			if (!buttons_now[i] && buttons_was[i] && m_active[i] >= 0)
			{
				m_m.send_note_off(static_cast<uint8_t>(m_active[i]));
				m_active[i] = -1;
			}
		}

		// Presses: play the fixed note for this button.
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			if (buttons_now[i] && !buttons_was[i])
			{
				if (m_active[i] >= 0)
					m_m.send_note_off(static_cast<uint8_t>(m_active[i]));
				int n = clamp_midi(scale_step_to_midi_with(
					scale_step_offset(i), m_tonic, mappings::SCALES[m_scale_index]));
				m_active[i] = n;
				m_m.send_note_on(static_cast<uint8_t>(n), static_cast<uint8_t>(m_current_velocity));
			}
		}
	}

	static int scale_step_to_midi_with(int step, int tonic, const mappings::Scale& s)
	{
		int n = s.interval_count;
		int octave = step / n;
		int idx = step - octave * n;
		if (idx < 0) { idx += n; octave -= 1; }
		return tonic + 12 * octave + s.intervals[idx];
	}

	// === CHORD: each button plays a chord. Two sub-modes toggled by QAM.
	// Polyphonic: hold multiple buttons to layer chords.
	void update_chord(const sc::SCState& cur, const sc::SCState& prev)
	{
		// L3/R3 behavior depends on sub-mode:
		// - default (scale-degree): cycle scales (major / minor / ...)
		// - root mode: shift tonic by perfect 5ths (circle-of-5ths key change)
		bool l3 = cur.l3 && !prev.l3;
		bool r3 = cur.r3 && !prev.r3;
		if (m_chord_root_mode)
		{
			if (l3) m_tonic -= 7;
			if (r3) m_tonic += 7;
			// Wrap within one octave (C3..B3) so the cycle stays in audible range.
			while (m_tonic > 71) m_tonic -= 12;
			while (m_tonic < 48) m_tonic += 12;
		}
		else
		{
			if (l3) m_scale_index = (m_scale_index + mappings::SCALE_COUNT - 1) % mappings::SCALE_COUNT;
			if (r3) m_scale_index = (m_scale_index + 1) % mappings::SCALE_COUNT;
		}
		// QAM toggles between default (scale-degree chords) and root mode.
		if (cur.qam && !prev.qam)
			m_chord_root_mode = !m_chord_root_mode;
		if (cur.steam && !prev.steam)
			m_alt_tonic = m_tonic;

		bool buttons_now[NUM_BUTTONS] = {
			cur.dpad_down, cur.dpad_right, cur.dpad_left, cur.dpad_up,
			cur.a, cur.b, cur.x, cur.y,
			cur.view, cur.menu,
			cur.l4, cur.r4, cur.l5, cur.r5,
			cur.lb, cur.rb
		};
		bool buttons_was[NUM_BUTTONS] = {
			prev.dpad_down, prev.dpad_right, prev.dpad_left, prev.dpad_up,
			prev.a, prev.b, prev.x, prev.y,
			prev.view, prev.menu,
			prev.l4, prev.r4, prev.l5, prev.r5,
			prev.lb, prev.rb
		};

		const mappings::Scale& s = mappings::SCALES[m_scale_index];

		// Releases: stop every voice for buttons no longer held.
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			if (!buttons_now[i] && buttons_was[i])
			{
				for (int v = 0; v < m_chord_voice_count[i]; ++v)
				{
					if (m_chord_notes[i][v] >= 0)
						m_m.send_note_off(static_cast<uint8_t>(m_chord_notes[i][v]));
					m_chord_notes[i][v] = -1;
				}
				m_chord_voice_count[i] = 0;
			}
		}

		// Presses: build a chord. In default mode, triad on the button's
		// scale-step. In root mode, the button's chord-quality voicing
		// rooted at the tonic.
		for (int i = 0; i < NUM_BUTTONS; ++i)
		{
			if (buttons_now[i] && !buttons_was[i])
			{
				// Stop any straggler voices for this button first.
				for (int v = 0; v < m_chord_voice_count[i]; ++v)
				{
					if (m_chord_notes[i][v] >= 0)
						m_m.send_note_off(static_cast<uint8_t>(m_chord_notes[i][v]));
				}
				int size = 0;
				int notes[mappings::CHORD_MAX_VOICES] = {};
				if (m_chord_root_mode)
				{
					// Composition palette: scale-degree functional chord.
					const mappings::CompChord& cc = mappings::COMP_CHORDS[i];
					size = cc.voice_count;
					if (size > mappings::CHORD_MAX_VOICES) size = mappings::CHORD_MAX_VOICES;
					for (int j = 0; j < size; ++j)
					{
						int voice_step = cc.root_step + cc.voice_offsets[j];
						notes[j] = clamp_midi(scale_step_to_midi_with(voice_step, m_tonic, s));
					}
				}
				else
				{
					// Default: triad rooted at the button's scale-step offset.
					int root_step = scale_step_offset(i);
					size = 3;
					for (int j = 0; j < size; ++j)
					{
						int voice_step = root_step + j * 2; // stack of 3rds
						notes[j] = clamp_midi(scale_step_to_midi_with(voice_step, m_tonic, s));
					}
				}
				for (int j = 0; j < size; ++j)
				{
					m_chord_notes[i][j] = notes[j];
					m_m.send_note_on(static_cast<uint8_t>(notes[j]), static_cast<uint8_t>(m_current_velocity));
				}
				m_chord_voice_count[i] = size;
			}
		}
	}

	static int scale_step_offset(int button)
	{
		static constexpr int OFFSET[NUM_BUTTONS] = {
			mappings::SCALE_OFF_DPAD_DOWN, mappings::SCALE_OFF_DPAD_RIGHT,
			mappings::SCALE_OFF_DPAD_LEFT, mappings::SCALE_OFF_DPAD_UP,
			mappings::SCALE_OFF_A, mappings::SCALE_OFF_B,
			mappings::SCALE_OFF_X, mappings::SCALE_OFF_Y,
			mappings::SCALE_OFF_VIEW, mappings::SCALE_OFF_MENU,
			mappings::SCALE_OFF_L4, mappings::SCALE_OFF_R4,
			mappings::SCALE_OFF_L5, mappings::SCALE_OFF_R5,
			mappings::SCALE_OFF_LB, mappings::SCALE_OFF_RB,
		};
		return OFFSET[button];
	}

	// === LSDJ: tracker-ish monophonic. Two sub-modes toggled by QAM.
	//   QAM off (default): hold A = note sounds at current root. While A is
	//     held, shifters retrigger at new root. Without A, shifters silently
	//     move root (so you can pre-tune before pressing A).
	//   QAM on (every-press plays): each shifter press both moves root AND
	//     fires a note. Releasing the most-recently-pressed button stops it.
	// L3/R3 always reset root + stop note.
	void update_lsdj(const sc::SCState& cur, const sc::SCState& prev)
	{
		// QAM toggle: cleanly end any held note across the transition.
		if (cur.qam && !prev.qam)
		{
			m_lsdj_qam_mode = !m_lsdj_qam_mode;
			if (m_lsdj_note >= 0)
			{
				m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
				m_lsdj_note = -1;
			}
			m_lsdj_active = -1;
		}

		// Reset (L3/R3)
		bool reset = (cur.l3 && !prev.l3) || (cur.r3 && !prev.r3);
		if (reset)
		{
			if (m_lsdj_note >= 0) m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
			m_lsdj_note = -1;
			m_lsdj_active = -1;
			m_lsdj_root = mappings::LSDJ_DEFAULT_ROOT;
		}

		// Shifter table: now-pressed, was-pressed, offset, and a per-button id
		// used to track ownership of the currently-playing note in QAM mode.
		struct Shifter { bool now; bool was; int offset; int id; };
		const Shifter shifters[] = {
			{ cur.dpad_left,  prev.dpad_left,  mappings::LSDJ_OFF_DPAD_LEFT,  1 },
			{ cur.dpad_right, prev.dpad_right, mappings::LSDJ_OFF_DPAD_RIGHT, 2 },
			{ cur.dpad_down,  prev.dpad_down,  mappings::LSDJ_OFF_DPAD_DOWN,  3 },
			{ cur.dpad_up,    prev.dpad_up,    mappings::LSDJ_OFF_DPAD_UP,    4 },
			{ cur.x,          prev.x,          mappings::LSDJ_OFF_X,          5 },
			{ cur.b,          prev.b,          mappings::LSDJ_OFF_B,          6 },
			{ cur.lb,         prev.lb,         mappings::LSDJ_OFF_LB,         7 },
			{ cur.rb,         prev.rb,         mappings::LSDJ_OFF_RB,         8 },
			{ cur.l4,         prev.l4,         mappings::LSDJ_OFF_L4,         9 },
			{ cur.r4,         prev.r4,         mappings::LSDJ_OFF_R4,        10 },
			{ cur.l5,         prev.l5,         mappings::LSDJ_OFF_L5,        11 },
			{ cur.r5,         prev.r5,         mappings::LSDJ_OFF_R5,        12 },
			{ cur.menu,       prev.menu,       mappings::LSDJ_OFF_MENU,      13 },
			{ cur.view,       prev.view,       mappings::LSDJ_OFF_VIEW,      14 },
		};

		int total_shift = 0;
		int last_pressed_shifter_id = -1;
		for (const auto& s : shifters)
		{
			if (s.now && !s.was)
			{
				total_shift += s.offset;
				last_pressed_shifter_id = s.id;
			}
		}
		bool shifter_pressed = (last_pressed_shifter_id != -1);
		if (shifter_pressed)
			m_lsdj_root = clamp_midi(m_lsdj_root + total_shift);

		bool a_press = cur.a && !prev.a;
		bool a_release = !cur.a && prev.a;

		if (!m_lsdj_qam_mode)
		{
			// === Default sub-mode: only A triggers notes ===
			if (a_press)
			{
				if (m_lsdj_note >= 0) m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
				m_lsdj_note = m_lsdj_root;
				m_m.send_note_on(static_cast<uint8_t>(m_lsdj_note), static_cast<uint8_t>(m_current_velocity));
			}
			else if (a_release && m_lsdj_note >= 0)
			{
				m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
				m_lsdj_note = -1;
			}
			else if (shifter_pressed && cur.a && m_lsdj_note >= 0)
			{
				m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
				m_lsdj_note = m_lsdj_root;
				m_m.send_note_on(static_cast<uint8_t>(m_lsdj_note), static_cast<uint8_t>(m_current_velocity));
			}
		}
		else
		{
			// === QAM-on sub-mode: every press plays. Release of the active
			// button stops the note - UNLESS A is held, in which case A acts
			// as a legato sustain (note keeps ringing through shifter releases
			// until A itself is released).
			int newly_active = -1; // 0 = A, 1..14 = shifters
			if (a_press) newly_active = 0;
			if (shifter_pressed) newly_active = last_pressed_shifter_id;
			if (newly_active != -1)
			{
				if (m_lsdj_note >= 0) m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
				m_lsdj_note = m_lsdj_root;
				m_m.send_note_on(static_cast<uint8_t>(m_lsdj_note), static_cast<uint8_t>(m_current_velocity));
				m_lsdj_active = newly_active;
			}

			if (a_release && m_lsdj_note >= 0)
			{
				// A release always stops the note (even if a shifter is also held).
				m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
				m_lsdj_note = -1;
				m_lsdj_active = -1;
			}
			else
			{
				// Shifter release: stop note unless A is held (legato sustain).
				for (const auto& s : shifters)
				{
					if (!s.now && s.was && s.id == m_lsdj_active)
					{
						if (cur.a)
						{
							// A is held - sustain. Note keeps ringing; A
							// takes over as the active button so its release
							// is what eventually stops the note.
							m_lsdj_active = 0;
						}
						else if (m_lsdj_note >= 0)
						{
							m_m.send_note_off(static_cast<uint8_t>(m_lsdj_note));
							m_lsdj_note = -1;
							m_lsdj_active = -1;
						}
						break;
					}
				}
			}
		}
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

	// SCALE / CHORD mode shared state: fixed tonic + selectable scale.
	int m_scale_index = mappings::SCALE_DEFAULT_INDEX;
	int m_tonic = mappings::SCALE_DEFAULT_TONIC;
	int m_alt_tonic = mappings::SCALE_DEFAULT_ALT_TONIC;

	// CHORD mode: per-button voice buffer for polyphonic chord playback,
	// plus a flag for the root-mode sub-toggle (QAM toggles this).
	int m_chord_notes[NUM_BUTTONS][mappings::CHORD_MAX_VOICES] = {};
	int m_chord_voice_count[NUM_BUTTONS] = {};
	bool m_chord_root_mode = false;

	// LSDJ mode: monophonic latched playback.
	int m_lsdj_root = mappings::LSDJ_DEFAULT_ROOT;
	int m_lsdj_note = -1;
	bool m_lsdj_qam_mode = false; // QAM toggles "every press plays" sub-mode
	int m_lsdj_active = -1;       // which button (by ID) owns the current note in QAM mode
};

// DAW mode: a separate mode where buttons send DAW commands (one CC per
// action on mappings::DAW_MIDI_CHANNEL, read by the Bitwig script in
// bitwig/). While enabled, the regular CC + note output is off entirely.
class DawMode
{
public:
	explicit DawMode(MidiEmitter& out) : m_out(&out) {}

	bool enabled() const { return m_enabled; }
	mappings::DawAction last_action() const { return m_last_action; }

	// Switch where DAW commands go (e.g. once the DAW port shows up).
	void set_output(MidiEmitter& out) { m_out = &out; }

	void set_enabled(bool on)
	{
		if (on == m_enabled) return;
		m_enabled = on;
		// Don't leave a DAW CC latched at 127 if a button was held.
		for (const auto& b : mappings::DAW_BINDINGS)
		{
			if (is_sent(b.action)) m_out->send_button(static_cast<uint8_t>(b.action), false);
		}
	}

	// Returns true if the user asked to leave DAW mode (EXIT_DAW_MODE).
	bool update(const sc::SCState& cur, const sc::SCState& prev)
	{
		bool exit_requested = false;
		for (const auto& b : mappings::DAW_BINDINGS)
		{
			bool now = button(cur, b.button);
			bool was = button(prev, b.button);
			if (now == was) continue;

			if (b.action == mappings::DawAction::EXIT_DAW_MODE)
			{
				if (now) exit_requested = true;
				continue;
			}
			if (!is_sent(b.action)) continue;
			m_out->send_button(static_cast<uint8_t>(b.action), now);
			if (now) m_last_action = b.action;
		}
		return exit_requested;
	}

	static const char* action_name(mappings::DawAction a)
	{
		using A = mappings::DawAction;
		switch (a)
		{
		case A::PLAY_STOP:        return "Play/Stop";
		case A::STOP:             return "Stop";
		case A::RECORD:           return "Rec";
		case A::RECORD_TAKE:      return "RecTake";
		case A::UNDO:             return "Undo";
		case A::JUMP_BACK:        return "-1bar";
		case A::JUMP_FORWARD:     return "+1bar";
		case A::JUMP_BACK_BIG:    return "-Nbars";
		case A::JUMP_FORWARD_BIG: return "+Nbars";
		case A::GOTO_START:       return "Start";
		case A::PREV_MARKER:      return "PrevMark";
		case A::NEXT_MARKER:      return "NextMark";
		case A::ADD_MARKER:       return "AddMark";
		case A::TOGGLE_LOOP:      return "Loop";
		case A::TOGGLE_METRONOME: return "Click";
		case A::SELECT_LEFT:      return "ClipLeft";
		case A::SELECT_RIGHT:     return "ClipRight";
		case A::SELECT_AT_CURSOR: return "ClipAtCursor";
		case A::TRACK_PREV:       return "TrackUp";
		case A::TRACK_NEXT:       return "TrackDown";
		case A::EXIT_DAW_MODE:    return "Exit";
		default:                  return "-";
		}
	}

	static const char* button_name(mappings::DawButton b)
	{
		using B = mappings::DawButton;
		switch (b)
		{
		case B::LPAD_CLICK:  return "LPad";
		case B::RPAD_CLICK:  return "RPad";
		case B::A:           return "A";
		case B::B:           return "B";
		case B::X:           return "X";
		case B::Y:           return "Y";
		case B::LB:          return "LB";
		case B::RB:          return "RB";
		case B::L3:          return "L3";
		case B::R3:          return "R3";
		case B::VIEW:        return "View";
		case B::MENU:        return "Menu";
		case B::STEAM:       return "Steam";
		case B::QAM:         return "QAM";
		case B::DPAD_UP:     return "DUp";
		case B::DPAD_DOWN:   return "DDown";
		case B::DPAD_LEFT:   return "DLeft";
		case B::DPAD_RIGHT:  return "DRight";
		case B::L4:          return "L4";
		case B::L5:          return "L5";
		case B::R4:          return "R4";
		case B::R5:          return "R5";
		case B::LTRIG_CLICK: return "LTrigC";
		case B::RTRIG_CLICK: return "RTrigC";
		}
		return "?";
	}

private:
	static bool is_sent(mappings::DawAction a)
	{
		return a != mappings::DawAction::NONE && a != mappings::DawAction::EXIT_DAW_MODE;
	}

	static bool button(const sc::SCState& s, mappings::DawButton b)
	{
		using B = mappings::DawButton;
		switch (b)
		{
		case B::LPAD_CLICK:  return s.lpad_click;
		case B::RPAD_CLICK:  return s.rpad_click;
		case B::A:           return s.a;
		case B::B:           return s.b;
		case B::X:           return s.x;
		case B::Y:           return s.y;
		case B::LB:          return s.lb;
		case B::RB:          return s.rb;
		case B::L3:          return s.l3;
		case B::R3:          return s.r3;
		case B::VIEW:        return s.view;
		case B::MENU:        return s.menu;
		case B::STEAM:       return s.steam;
		case B::QAM:         return s.qam;
		case B::DPAD_UP:     return s.dpad_up;
		case B::DPAD_DOWN:   return s.dpad_down;
		case B::DPAD_LEFT:   return s.dpad_left;
		case B::DPAD_RIGHT:  return s.dpad_right;
		case B::L4:          return s.l4;
		case B::L5:          return s.l5;
		case B::R4:          return s.r4;
		case B::R5:          return s.r5;
		case B::LTRIG_CLICK: return s.ltrig_click;
		case B::RTRIG_CLICK: return s.rtrig_click;
		}
		return false;
	}

	MidiEmitter* m_out;
	bool m_enabled = mappings::DAW_MODE_DEFAULT_ON;
	mappings::DawAction m_last_action = mappings::DawAction::NONE;
};

// Everything analog or ambient: sticks, pads, triggers, pitch bend, tilt,
// gyro/accel, pad pressure and capacitive touches. These are muted in learn
// mode, because they stream constantly and swamp a DAW's MIDI learn.
static void emit_continuous(MidiEmitter& m, const sc::SCState& s)
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

	// AccY tilt -> Expression (CC 11). Continuous version of the velocity
	// mapping: flat/down = 127, tilted up past the deadzone reduces toward 0.
	if constexpr (ENABLE_ACCY_EXPRESSION)
	{
		int ay = s.accel_y;
		uint8_t expr = 127;
		if (ay >= ACCY_VELOCITY_DEADZONE)
		{
			float t = static_cast<float>(ay - ACCY_VELOCITY_DEADZONE)
				/ static_cast<float>(ACCY_VELOCITY_MAX_RAW - ACCY_VELOCITY_DEADZONE);
			if (t < 0.0f) t = 0.0f;
			if (t > 1.0f) t = 1.0f;
			int v = 127 - static_cast<int>(std::round(t * 127.0f));
			if (v < 0) v = 0;
			expr = static_cast<uint8_t>(v);
		}
		m.send(CC_EXPRESSION, expr);
	}

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

	// Pad pressure + capacitive touches
	if (!FREEZE_TRACKPADS_ON_RELEASE || s.lpad_touch)
		m.send_unipolar(CC_LPAD_PRESSURE, s.lpad_pressure);
	if (!FREEZE_TRACKPADS_ON_RELEASE || s.rpad_touch)
		m.send_unipolar(CC_RPAD_PRESSURE, s.rpad_pressure);
	m.send_button(CC_LPAD_TOUCH, s.lpad_touch);
	m.send_button(CC_RPAD_TOUCH, s.rpad_touch);
	m.send_button(CC_LGRIP_TOUCH, s.lgrip_touch);
	m.send_button(CC_RGRIP_TOUCH, s.rgrip_touch);
	m.send_button(CC_LSTICK_TOUCH, s.lstick_touch);
	m.send_button(CC_RSTICK_TOUCH, s.rstick_touch);
}

// Physical button presses (127 on press, 0 on release).
static void emit_buttons(MidiEmitter& m, const sc::SCState& s)
{
	using namespace mappings;

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

	// Triton-specific clicks/paddles/QAM
	m.send_button(CC_LPAD_CLICK, s.lpad_click);
	m.send_button(CC_RPAD_CLICK, s.rpad_click);
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

static void redraw(const sc::SCState& s, const MidiEmitter& m, const NoteEmitter& n,
	const DawMode& d, const std::string& daw_port, bool idle, bool learn)
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
		auto layout = n.layout();
		const char* mode_name =
			layout == mappings::NoteLayout::DIATONIC  ? "DIATONIC " :
			layout == mappings::NoteLayout::CHROMATIC ? "CHROMATIC" :
			layout == mappings::NoteLayout::SCALE     ? "SCALE    " :
			layout == mappings::NoteLayout::CHORD     ? "CHORD    " :
			                                            "LSDJ     ";
		os << "  Notes:    mode=" << mode_name << " (press 1-5)  ";
		if (layout == mappings::NoteLayout::DIATONIC)
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
		else if (layout == mappings::NoteLayout::CHROMATIC)
		{
			os << "root=" << n.root() << " home=" << n.home_root()
				<< " alt=" << n.alt_root() << " vel=" << n.current_velocity()
				<< " playing=";
			if (n.mono_note() >= 0) os << n.mono_note();
			else os << "-";
		}
		else if (layout == mappings::NoteLayout::LSDJ)
		{
			os << "root=" << n.lsdj_root()
				<< " vel=" << n.current_velocity()
				<< " playing=";
			if (n.lsdj_note() >= 0) os << n.lsdj_note();
			else os << "-";
			os << (n.lsdj_qam_mode()
				? "  EVERY-PRESS (QAM)"
				: "  hold-A");
			os << "  L3/R3=reset";
		}
		else
		{
			os << "scale=" << n.scale_name()
				<< " tonic=" << n.scale_tonic()
				<< " vel=" << n.current_velocity();
			if (layout == mappings::NoteLayout::CHORD)
			{
				if (n.chord_root_mode())
					os << "  PALETTE  L3/R3=key (5ths)  QAM=toggle";
				else
					os << "  scale-degree  L3/R3=scale  QAM=toggle";
			}
			else
				os << "  alt=" << n.scale_alt_tonic() << "  L3/R3=cycle scale";
		}
		os << "\033[K\n";
	}
	os << "  DAW mode: "
		<< (d.enabled() ? "\033[35mON \033[0m" : "OFF")
		<< " (press D)  -> " << daw_port << " ch" << (mappings::DAW_MIDI_CHANNEL + 1)
		<< "  last=" << DawMode::action_name(d.last_action()) << "\033[K\n";
	os << "            ";
	if (d.enabled())
	{
		for (const auto& b : mappings::DAW_BINDINGS)
		{
			if (b.action == mappings::DawAction::NONE) continue;
			os << DawMode::button_name(b.button) << "=" << DawMode::action_name(b.action) << " ";
		}
	}
	os << "\033[K\n";
	os << "  Learn:    "
		<< (learn ? "\033[36mON  - continuous CCs muted, only buttons are sent\033[0m" : "OFF")
		<< " (press L)\033[K\n";
	os << "  Controller: "
		<< (idle ? "\033[33mNO INPUT - asleep or off? Press a button to wake it.\033[0m" : "connected")
		<< "\033[K\n";
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
	platform::init();
	std::atexit(platform::cleanup);
	platform::install_signal_handler();

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
		platform::pause_before_exit();
		return 1;
	}

	sc::SteamController controller;
	if (!wait_for_controller(controller))
	{
		hid_exit();
		return 0;
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
		platform::pause_before_exit();
		return 1;
	}

	std::cout << "Lizard mode disabled + IMU streaming enabled.\n";
	std::cout << "Sending MIDI CC on channel " << static_cast<int>(mappings::MIDI_CHANNEL + 1)
		<< ". Live dashboard (Ctrl+C to exit):\n\n";

	// DAW commands go to their own port when one matches DAW_PORT_MATCH (e.g.
	// a second loopMIDI port "loopMIDI DAW"), so they never reach instruments.
	// Until such a port exists they share the main port; we look again every
	// time DAW mode is entered, so creating the port later just works.
	libremidi::midi_out daw_midi;
	std::string daw_port_name = out_port->port_name + " (shared, no \"DAW\" port found)";
	bool daw_separate = false;

	MidiEmitter emitter(midi, mappings::MIDI_CHANNEL);
	MidiEmitter daw_shared_emitter(midi, mappings::DAW_MIDI_CHANNEL);
	MidiEmitter daw_port_emitter(daw_midi, mappings::DAW_MIDI_CHANNEL);
	NoteEmitter notes(emitter);
	DawMode daw(daw_shared_emitter);

	auto find_daw_port = [&]()
	{
		if (daw_separate) return;
		libremidi::observer obs;
		for (const libremidi::output_port& p : obs.get_output_ports())
		{
			if (p.port_name == out_port->port_name || !icontains(p.port_name, mappings::DAW_PORT_MATCH))
				continue;
			daw_midi.open_port(p);
			if (!daw_midi.is_port_open()) continue;
			daw_port_name = p.port_name;
			daw_separate = true;
			daw.set_output(daw_port_emitter);
			return;
		}
	};
	find_daw_port();
	std::cout << "DAW mode commands -> " << daw_port_name << '\n';

	auto set_daw_mode = [&](bool on)
	{
		if (on == daw.enabled()) return;
		if (on)
		{
			find_daw_port();
			// Leaving the instrument: silence notes and release button CCs.
			notes.panic();
			emit_buttons(emitter, sc::SCState{});
		}
		daw.set_enabled(on);
	};

	auto last_lizard_refresh = std::chrono::steady_clock::now();
	auto last_redraw = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	sc::SCState state;
	sc::SCState prev_state;
	bool have_prev = false;
	auto last_report = std::chrono::steady_clock::now();
	bool idle = false; // no reports for a while: controller asleep / off
	bool learn_mode = false; // 'L': mute continuous CCs so DAW MIDI learn sees only buttons
	while (true)
	{
		auto now = std::chrono::steady_clock::now();
		if (now - last_lizard_refresh >= std::chrono::seconds(3))
		{
			controller.keep_lizard_mode_disabled();
			last_lizard_refresh = now;
		}

		auto r = controller.read(100);
		if (!r || !sc::parse_triton_report(r->bytes.data(), r->bytes.size(), state))
		{
			// The controller streams continuously while awake, so a gap means
			// it went to sleep or was switched off.
			now = std::chrono::steady_clock::now();
			if (now - last_report >= std::chrono::seconds(2)) idle = true;
			if (idle && now - last_redraw >= std::chrono::milliseconds(250))
			{
				redraw(prev_state, emitter, notes, daw, daw_port_name, idle, learn_mode);
				last_redraw = now;
			}
			continue;
		}
		last_report = now;
		if (idle)
		{
			// Woke back up: it may have reconnected with default settings
			// (lizard mode on, IMU off), so re-apply ours.
			controller.configure_for_raw_input();
			idle = false;
		}

		// Keyboard mode switching: focus the console and press 1-5, D = DAW mode.
		int c;
		while (platform::poll_key(c))
		{
			if (c == '1') notes.set_layout(mappings::NoteLayout::DIATONIC);
			else if (c == '2') notes.set_layout(mappings::NoteLayout::CHROMATIC);
			else if (c == '3') notes.set_layout(mappings::NoteLayout::SCALE);
			else if (c == '4') notes.set_layout(mappings::NoteLayout::CHORD);
			else if (c == '5') notes.set_layout(mappings::NoteLayout::LSDJ);
			else if (c == 'd' || c == 'D') set_daw_mode(!daw.enabled());
			else if (c == 'l' || c == 'L') learn_mode = !learn_mode;
		}

		if (daw.enabled())
		{
			// DAW mode: buttons send DAW commands only.
			if (have_prev && daw.update(state, prev_state)) set_daw_mode(false);
		}
		else
		{
			if (!learn_mode) emit_continuous(emitter, state);
			emit_buttons(emitter, state);
			if constexpr (mappings::ENABLE_NOTES)
			{
				if (have_prev) notes.update(state, prev_state);
			}
		}
		prev_state = state;
		have_prev = true;

		if (now - last_redraw >= std::chrono::milliseconds(REDRAW_INTERVAL_MS))
		{
			redraw(state, emitter, notes, daw, daw_port_name, idle, learn_mode);
			last_redraw = now;
		}
	}

	midi.close_port();
	if (daw_separate) daw_midi.close_port();
	hid_exit();
	return 0;
}
