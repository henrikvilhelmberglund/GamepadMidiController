#include "steam_controller.h"

#include <hidapi.h>
#include <array>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <cwchar>

#include <Windows.h>
extern "C" {
#include <hidsdi.h>
}
#pragma comment(lib, "hid.lib")

namespace sc
{

SteamController::~SteamController()
{
	if (m_dev)
	{
		hid_close(m_dev);
		m_dev = nullptr;
	}
}

static std::string wide_to_utf8(const wchar_t* w)
{
	if (!w) return "";
	std::string s;
	for (; *w; ++w) s.push_back(*w < 128 ? static_cast<char>(*w) : '?');
	return s;
}

// Wireless firmware quirk (per SDL): SetFeature/GetFeature may return failure
// when it really means "pending radio roundtrip". Retry up to ~25ms.
static int retrying_send_feature(hid_device* dev, uint8_t* buf, size_t len)
{
	for (int i = 0; i < 50; ++i)
	{
		int r = hid_send_feature_report(dev, buf, len);
		if (r >= 0) return r;
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
	return -1;
}

static int retrying_get_feature(hid_device* dev, uint8_t* buf, size_t len)
{
	for (int i = 0; i < 50; ++i)
	{
		int r = hid_get_feature_report(dev, buf, len);
		if (r >= 0) return r;
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
	return -1;
}

static void build_set_settings(std::array<uint8_t, TRITON_FEATURE_REPORT_SIZE>& buf,
	std::initializer_list<std::pair<uint8_t, uint16_t>> settings);

struct HidCaps
{
	USHORT input_len = 0;
	USHORT output_len = 0;
	USHORT feature_len = 0;
};

static HidCaps query_hid_caps(const char* path)
{
	HidCaps caps;
	HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return caps;
	PHIDP_PREPARSED_DATA pp = nullptr;
	if (HidD_GetPreparsedData(h, &pp))
	{
		HIDP_CAPS hc{};
		if (HidP_GetCaps(pp, &hc) == HIDP_STATUS_SUCCESS)
		{
			caps.input_len = hc.InputReportByteLength;
			caps.output_len = hc.OutputReportByteLength;
			caps.feature_len = hc.FeatureReportByteLength;
		}
		HidD_FreePreparsedData(pp);
	}
	CloseHandle(h);
	return caps;
}

void SteamController::list_valve_devices()
{
	std::cout << "All Valve (VID=0x28de) HID interfaces:\n";
	hid_device_info* list = hid_enumerate(VALVE_VID, 0);
	if (!list)
	{
		std::cout << "  (none found - hid_enumerate returned null)\n";
		return;
	}
	for (hid_device_info* it = list; it; it = it->next)
	{
		HidCaps caps = query_hid_caps(it->path);
		std::cout << "  PID=0x" << std::hex << std::setw(4) << std::setfill('0') << it->product_id
			<< " IF=" << std::dec << it->interface_number
			<< " usage_page=0x" << std::hex << it->usage_page
			<< " usage=0x" << it->usage
			<< std::dec << std::setfill(' ')
			<< " in=" << caps.input_len
			<< " out=" << caps.output_len
			<< " feat=" << caps.feature_len
			<< " product=\"" << wide_to_utf8(it->product_string) << "\""
			<< "\n    path=" << it->path << "\n";
	}
	hid_free_enumeration(list);
}

bool SteamController::open_first()
{
	// Wireless dongles ("Puck" PID 0x1304, dongle PID 0x1142) expose one
	// gamepad collection per controller slot. Only one slot is actually paired
	// to a live controller; the others return empty reads. We try each candidate,
	// configure it, and probe for input data. The first one that yields a real
	// report wins.
	hid_device_info* list = hid_enumerate(VALVE_VID, 0);
	for (hid_device_info* it = list; it; it = it->next)
	{
		bool known_pid = (it->product_id == SC_PID_WIRED
			|| it->product_id == SC_PID_DONGLE
			|| it->product_id == SC_PID_PUCK);
		if (!known_pid) continue;

		// Vendor-defined gamepad collection: usage_page=0xFF00, usage=0x01.
		// Skip keyboard/mouse collections and the dongle management interface
		// (which has usage=0x02).
		if (it->usage_page != SC_GAMEPAD_USAGE_PAGE) continue;
		if (it->usage != SC_GAMEPAD_USAGE) continue;

		hid_device* dev = hid_open_path(it->path);
		if (!dev) continue;

		std::cout << "  trying " << it->path << " ... ";
		std::cout.flush();

		// Probe: send SETTING_LIZARD_MODE=OFF with Report ID 1 (Triton/Puck protocol).
		// Empty dongle slots reject the feature report; the paired slot accepts it.
		std::array<uint8_t, TRITON_FEATURE_REPORT_SIZE> probe_buf{};
		build_set_settings(probe_buf, { { SETTING_LIZARD_MODE, LIZARD_MODE_OFF } });
		int sres = retrying_send_feature(dev, probe_buf.data(), probe_buf.size());
		if (sres < 0)
		{
			const wchar_t* err = hid_error(dev);
			std::cout << "no controller on this slot (" << wide_to_utf8(err) << ")\n";
			hid_close(dev);
			continue;
		}

		std::cout << "ACCEPTED (likely paired)\n";
		m_dev = dev;
		m_path = it->path;
		m_pid = it->product_id;
		hid_free_enumeration(list);
		hid_set_nonblocking(m_dev, 0);
		return true;
	}
	hid_free_enumeration(list);
	return false;
}

// Build a SET_SETTINGS_VALUES feature report for the Triton/Puck protocol:
// byte 0 = Report ID 1, byte 1 = command, byte 2 = payload length, byte 3+ = settings.
static void build_set_settings(std::array<uint8_t, TRITON_FEATURE_REPORT_SIZE>& buf,
	std::initializer_list<std::pair<uint8_t, uint16_t>> settings)
{
	buf.fill(0);
	buf[0] = TRITON_FEATURE_REPORT_ID;
	buf[1] = ID_SET_SETTINGS_VALUES;
	buf[2] = static_cast<uint8_t>(settings.size() * 3);
	int i = 0;
	for (auto& [tag, value] : settings)
	{
		buf[3 + i * 3 + 0] = tag;
		buf[3 + i * 3 + 1] = static_cast<uint8_t>(value & 0xFF);
		buf[3 + i * 3 + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
		++i;
	}
}

bool SteamController::send_feature(const uint8_t* data, size_t len)
{
	// Windows HID requires the buffer to exactly match FeatureReportByteLength.
	std::array<uint8_t, TRITON_FEATURE_REPORT_SIZE> buf{};
	std::memcpy(buf.data(), data, len);
	int res = retrying_send_feature(m_dev, buf.data(), buf.size());
	return res >= 0;
}

bool SteamController::keep_lizard_mode_disabled()
{
	if (!m_dev) return false;
	std::array<uint8_t, TRITON_FEATURE_REPORT_SIZE> buf{};
	build_set_settings(buf, { { SETTING_LIZARD_MODE, LIZARD_MODE_OFF } });
	return retrying_send_feature(m_dev, buf.data(), buf.size()) >= 0;
}

bool SteamController::configure_for_raw_input()
{
	if (!m_dev) return false;

	// 1. Disable lizard mode (stops keyboard/mouse emulation).
	if (!keep_lizard_mode_disabled())
	{
		std::cerr << "SET_SETTINGS_VALUES(LIZARD_MODE_OFF) failed: "
			<< wide_to_utf8(hid_error(m_dev)) << "\n";
		return false;
	}

	// 2. Enable IMU streaming (raw gyro + raw accel).
	{
		std::array<uint8_t, TRITON_FEATURE_REPORT_SIZE> buf{};
		build_set_settings(buf, {
			{ SETTING_IMU_MODE, IMU_MODE_SEND_RAW_GYRO | IMU_MODE_SEND_RAW_ACCEL }
		});
		if (retrying_send_feature(m_dev, buf.data(), buf.size()) < 0)
		{
			std::cerr << "SET_SETTINGS_VALUES(IMU_MODE) failed: "
				<< wide_to_utf8(hid_error(m_dev)) << "\n";
			return false;
		}
	}

	return true;
}

std::optional<RawReport> SteamController::read(int timeout_ms)
{
	if (!m_dev) return std::nullopt;
	std::array<uint8_t, 64> buf{};
	int res = hid_read_timeout(m_dev, buf.data(), buf.size(), timeout_ms);
	if (res <= 0) return std::nullopt;
	RawReport r;
	r.bytes.assign(buf.begin(), buf.begin() + res);
	return r;
}

} // namespace sc
