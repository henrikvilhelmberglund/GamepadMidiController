#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct hid_device_;
typedef struct hid_device_ hid_device;

namespace sc
{

// Valve VID and known Steam Controller PIDs.
constexpr uint16_t VALVE_VID = 0x28de;
constexpr uint16_t SC_PID_WIRED = 0x1102;
constexpr uint16_t SC_PID_DONGLE = 0x1142;
constexpr uint16_t SC_PID_PUCK = 0x1304; // newer wireless receiver

// Vendor-defined HID usage page + usage used by the gamepad interface.
constexpr uint16_t SC_GAMEPAD_USAGE_PAGE = 0xFF00;
constexpr uint16_t SC_GAMEPAD_USAGE = 0x0001;

// Feature-report IDs (subset; from Valve's controller_constants.h via SDL).
constexpr uint8_t ID_CLEAR_DIGITAL_MAPPINGS = 0x81;
constexpr uint8_t ID_GET_ATTRIBUTES_VALUES  = 0x83;
constexpr uint8_t ID_SET_SETTINGS_VALUES    = 0x87;
constexpr uint8_t ID_LOAD_DEFAULT_SETTINGS  = 0x8E;

// Settings-enum indices (subset).
constexpr uint8_t SETTING_LEFT_TRACKPAD_MODE     = 7;
constexpr uint8_t SETTING_RIGHT_TRACKPAD_MODE    = 8;
constexpr uint8_t SETTING_LIZARD_MODE            = 9;
constexpr uint8_t SETTING_IMU_MODE               = 48;
constexpr uint8_t SETTING_WIRELESS_PACKET_VERSION = 49;

// Lizard mode values.
constexpr uint16_t LIZARD_MODE_OFF = 0;
constexpr uint16_t LIZARD_MODE_ON  = 1;

// Trackpad mode values.
constexpr uint16_t TRACKPAD_ABSOLUTE_MOUSE = 0;
constexpr uint16_t TRACKPAD_NONE = 7;

// IMU mode flag bits.
constexpr uint16_t IMU_MODE_OFF                = 0x0000;
constexpr uint16_t IMU_MODE_SEND_ORIENTATION   = 0x0004;
constexpr uint16_t IMU_MODE_SEND_RAW_ACCEL     = 0x0008;
constexpr uint16_t IMU_MODE_SEND_RAW_GYRO      = 0x0010;

// Feature reports on the Triton/Puck use Report ID 1 (the original SC uses ID 0).
constexpr uint8_t TRITON_FEATURE_REPORT_ID = 0x01;
constexpr size_t TRITON_FEATURE_REPORT_SIZE = 64;

struct RawReport
{
	std::vector<uint8_t> bytes;
};

class SteamController
{
public:
	SteamController() = default;
	~SteamController();

	SteamController(const SteamController&) = delete;
	SteamController& operator=(const SteamController&) = delete;

	// Enumerate Valve HID devices and open the first SC gamepad interface.
	// Returns false on no-device-found or open failure.
	bool open_first();

	// Print every Valve HID interface to stdout (for diagnosing why open_first
	// can't find the gamepad interface).
	static void list_valve_devices();

	// Send the feature reports that take the SC out of "lizard mode"
	// (keyboard/mouse emulation) and enable IMU (gyro + accel + orientation).
	bool configure_for_raw_input();

	// Triton's firmware watchdog re-enables lizard mode every few seconds.
	// Call this periodically (every ~3s) to keep raw input flowing.
	bool keep_lizard_mode_disabled();

	// Blocking read with timeout (ms). Returns the report bytes on success,
	// std::nullopt on timeout or error. On Windows, byte 0 may be the report ID
	// (depends on hidapi build), so callers shouldn't assume a fixed offset yet.
	std::optional<RawReport> read(int timeout_ms = 100);

	const std::string& path() const { return m_path; }
	uint16_t product_id() const { return m_pid; }
	bool is_open() const { return m_dev != nullptr; }

private:
	hid_device* m_dev = nullptr;
	std::string m_path;
	uint16_t m_pid = 0;

	bool send_feature(const uint8_t* data, size_t len);
};

} // namespace sc
