#include "GamepadMidiController.h"
#include "steam_controller.h"

#include <hidapi.h>
#include <iomanip>

int main()
{
	std::cout << "GamepadMidiController - Phase 1 HIDAPI checkpoint\n";
	std::cout << "(MIDI output is disabled in this phase - just dumping raw report bytes)\n\n";

	if (hid_init() != 0)
	{
		std::cerr << "hid_init() failed\n";
		return 1;
	}

	sc::SteamController::list_valve_devices();
	std::cout << "\n";

	sc::SteamController controller;
	if (!controller.open_first())
	{
		std::cerr << "No Steam Controller gamepad interface found.\n"
			<< "Check the device list above - we look for PID 0x1102 (wired) or 0x1142 (dongle)\n"
			<< "on interface_number=2. If interfaces are listed but none match those criteria,\n"
			<< "tell me the actual PID + interface_number that looks like the gamepad.\n";
		hid_exit();
		return 1;
	}

	const char* kind = "unknown";
	switch (controller.product_id())
	{
	case sc::SC_PID_WIRED:  kind = "wired"; break;
	case sc::SC_PID_DONGLE: kind = "dongle"; break;
	case sc::SC_PID_PUCK:   kind = "puck (wireless receiver)"; break;
	}
	std::cout << "Opened SC: PID=0x" << std::hex << controller.product_id() << std::dec
		<< " (" << kind << ")\n";
	std::cout << "Path: " << controller.path() << "\n\n";

	if (!controller.configure_for_raw_input())
	{
		std::cerr << "Failed to disable lizard mode / enable IMU\n";
		hid_exit();
		return 1;
	}

	std::cout << "Lizard mode disabled + IMU streaming enabled.\n";
	std::cout << "Reading reports (Ctrl+C to exit)...\n\n";

	int report_count = 0;
	auto last_lizard_refresh = std::chrono::steady_clock::now();
	while (true)
	{
		// Triton firmware re-enables lizard mode after a few seconds of "idle".
		// Re-disable it every 3s to keep raw reports flowing.
		auto now = std::chrono::steady_clock::now();
		if (now - last_lizard_refresh >= std::chrono::seconds(3))
		{
			controller.keep_lizard_mode_disabled();
			last_lizard_refresh = now;
		}

		auto r = controller.read(1000);
		if (!r) continue;

		// Print at most every 30th report so the terminal stays readable.
		if (report_count++ % 30 != 0) continue;

		std::cout << "[#" << std::setw(6) << report_count << " len=" << r->bytes.size() << "] ";
		std::cout << std::hex << std::setfill('0');
		size_t n = r->bytes.size();
		if (n > 32) n = 32; // first 32 bytes is plenty to see the structure
		for (size_t i = 0; i < n; i++)
		{
			std::cout << std::setw(2) << static_cast<int>(r->bytes[i]) << ' ';
		}
		std::cout << std::dec << std::setfill(' ') << "\n";
	}

	hid_exit();
	return 0;
}
