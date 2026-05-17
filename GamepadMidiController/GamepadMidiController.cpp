#include "GamepadMidiController.h"
#include "steam_controller.h"

#include <hidapi.h>
#include <Windows.h>
#include <iomanip>
#include <sstream>

constexpr int BAR_WIDTH = 32;
constexpr int DASHBOARD_LINES = 26;
constexpr int REDRAW_INTERVAL_MS = 33; // ~30 Hz to avoid flashing

static void enable_vt_mode()
{
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD mode = 0;
	if (!GetConsoleMode(h, &mode)) return;
	SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

// Map a raw value (assumed in [-range, +range] for bipolar or [0, range] otherwise) to a 0..BAR_WIDTH index.
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

static void render(std::ostringstream& os, const char* label, int raw, int range, bool bipolar)
{
	os << "  " << std::left << std::setw(5) << label << std::right
		<< make_bar(raw, range, bipolar)
		<< " " << std::setw(7) << raw << "\033[K\n";
}

static bool g_dashboard_initialized = false;

static void redraw(const sc::SCState& s)
{
	std::ostringstream os;
	if (g_dashboard_initialized)
	{
		os << "\033[" << DASHBOARD_LINES << "A";
	}
	g_dashboard_initialized = true;

	os << "  Sticks / Triggers" << "\033[K\n";
	render(os, "LSX", s.lstick_x, 32767, true);
	render(os, "LSY", s.lstick_y, 32767, true);
	render(os, "RSX", s.rstick_x, 32767, true);
	render(os, "RSY", s.rstick_y, 32767, true);
	render(os, "LT",  s.ltrigger, 32767, false);
	render(os, "RT",  s.rtrigger, 32767, false);

	os << "  Trackpads" << "\033[K\n";
	render(os, "LPX", s.lpad_x, 32767, true);
	render(os, "LPY", s.lpad_y, 32767, true);
	render(os, "LPP", s.lpad_pressure, 32767, false);
	render(os, "RPX", s.rpad_x, 32767, true);
	render(os, "RPY", s.rpad_y, 32767, true);
	render(os, "RPP", s.rpad_pressure, 32767, false);

	os << "  Gyro / Accel" << "\033[K\n";
	render(os, "GyrX", s.gyro_x, 32767, true);
	render(os, "GyrY", s.gyro_y, 32767, true);
	render(os, "GyrZ", s.gyro_z, 32767, true);
	render(os, "AccX", s.accel_x, 32767, true);
	render(os, "AccY", s.accel_y, 32767, true);
	render(os, "AccZ", s.accel_z, 32767, true);

	os << "  Buttons: "
		<< "A:" << on_off(s.a) << "B:" << on_off(s.b) << "X:" << on_off(s.x) << "Y:" << on_off(s.y)
		<< " LB:" << on_off(s.lb) << "RB:" << on_off(s.rb)
		<< " L3:" << on_off(s.l3) << "R3:" << on_off(s.r3)
		<< "\033[K\n";
	os << "  Sys:     "
		<< "VIEW:" << on_off(s.view) << "MENU:" << on_off(s.menu)
		<< " STEAM:" << on_off(s.steam) << "QAM:" << on_off(s.qam)
		<< " DPAD U:" << on_off(s.dpad_up) << "D:" << on_off(s.dpad_down)
		<< "L:" << on_off(s.dpad_left) << "R:" << on_off(s.dpad_right)
		<< "\033[K\n";
	os << "  Touch:   "
		<< "LPad-T:" << on_off(s.lpad_touch) << "C:" << on_off(s.lpad_click)
		<< " RPad-T:" << on_off(s.rpad_touch) << "C:" << on_off(s.rpad_click)
		<< " LGrip:" << on_off(s.lgrip_touch) << " RGrip:" << on_off(s.rgrip_touch)
		<< "\033[K\n";
	os << "  Pull:    "
		<< "LTrigClick:" << on_off(s.ltrig_click) << " RTrigClick:" << on_off(s.rtrig_click)
		<< " LStick-T:" << on_off(s.lstick_touch) << " RStick-T:" << on_off(s.rstick_touch)
		<< "\033[K\n";
	os << "  Paddles: "
		<< "L4:" << on_off(s.l4) << "L5:" << on_off(s.l5)
		<< " R4:" << on_off(s.r4) << "R5:" << on_off(s.r5)
		<< "\033[K\n";

	std::cout << os.str();
	std::cout.flush();
}

int main()
{
	enable_vt_mode();

	std::cout << "GamepadMidiController - Phase 2: parsed dashboard (no MIDI yet)\n\n";

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
	std::cout << "\nOpened SC: PID=0x" << std::hex << controller.product_id() << std::dec
		<< " (" << kind << ")\n";

	if (!controller.configure_for_raw_input())
	{
		std::cerr << "Failed to disable lizard mode / enable IMU\n";
		hid_exit();
		return 1;
	}

	std::cout << "Lizard mode disabled + IMU streaming enabled.\n";
	std::cout << "Live dashboard (Ctrl+C to exit):\n\n";

	auto last_lizard_refresh = std::chrono::steady_clock::now();
	auto last_redraw = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	sc::SCState state;
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

		if (now - last_redraw >= std::chrono::milliseconds(REDRAW_INTERVAL_MS))
		{
			redraw(state);
			last_redraw = now;
		}
	}

	hid_exit();
	return 0;
}
