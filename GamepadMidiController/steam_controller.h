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

// Input report IDs.
constexpr uint8_t ID_TRITON_CONTROLLER_STATE     = 0x42;
constexpr uint8_t ID_TRITON_BATTERY_STATUS       = 0x43;
constexpr uint8_t ID_TRITON_CONTROLLER_STATE_BLE = 0x45;

// Triton button mask bits (within a 32-bit field).
constexpr uint32_t TRITON_BTN_A          = 0x00000001;
constexpr uint32_t TRITON_BTN_B          = 0x00000002;
constexpr uint32_t TRITON_BTN_X          = 0x00000004;
constexpr uint32_t TRITON_BTN_Y          = 0x00000008;
constexpr uint32_t TRITON_BTN_QAM        = 0x00000010;
constexpr uint32_t TRITON_BTN_R3         = 0x00000020;
constexpr uint32_t TRITON_BTN_VIEW       = 0x00000040;
constexpr uint32_t TRITON_BTN_R4         = 0x00000080;
constexpr uint32_t TRITON_BTN_R5         = 0x00000100;
constexpr uint32_t TRITON_BTN_RB         = 0x00000200;
constexpr uint32_t TRITON_BTN_DPAD_DOWN  = 0x00000400;
constexpr uint32_t TRITON_BTN_DPAD_RIGHT = 0x00000800;
constexpr uint32_t TRITON_BTN_DPAD_LEFT  = 0x00001000;
constexpr uint32_t TRITON_BTN_DPAD_UP    = 0x00002000;
constexpr uint32_t TRITON_BTN_MENU       = 0x00004000;
constexpr uint32_t TRITON_BTN_L3         = 0x00008000;
constexpr uint32_t TRITON_BTN_STEAM      = 0x00010000;
constexpr uint32_t TRITON_BTN_L4         = 0x00020000;
constexpr uint32_t TRITON_BTN_L5         = 0x00040000;
constexpr uint32_t TRITON_BTN_LB         = 0x00080000;
constexpr uint32_t TRITON_BTN_RSTICK_TOUCH = 0x00100000;
constexpr uint32_t TRITON_BTN_RPAD_TOUCH   = 0x00200000;
constexpr uint32_t TRITON_BTN_RPAD_CLICK   = 0x00400000;
constexpr uint32_t TRITON_BTN_RTRIG_CLICK  = 0x00800000;
constexpr uint32_t TRITON_BTN_LSTICK_TOUCH = 0x01000000;
constexpr uint32_t TRITON_BTN_LPAD_TOUCH   = 0x02000000;
constexpr uint32_t TRITON_BTN_LPAD_CLICK   = 0x04000000;
constexpr uint32_t TRITON_BTN_LTRIG_CLICK  = 0x08000000;
constexpr uint32_t TRITON_BTN_RGRIP_TOUCH  = 0x10000000;
constexpr uint32_t TRITON_BTN_LGRIP_TOUCH  = 0x20000000;

// Parsed controller state (one snapshot from one input report).
struct SCState
{
	uint8_t seq_num = 0;
	uint32_t buttons_raw = 0;

	// Digital
	bool a = false, b = false, x = false, y = false;
	bool lb = false, rb = false;
	bool l3 = false, r3 = false;           // stick clicks
	bool view = false, menu = false;       // back/start
	bool steam = false, qam = false;       // guide-style
	bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
	bool l4 = false, r4 = false, l5 = false, r5 = false;
	bool lpad_touch = false, rpad_touch = false;
	bool lpad_click = false, rpad_click = false;
	bool ltrig_click = false, rtrig_click = false;
	bool lstick_touch = false, rstick_touch = false;
	bool lgrip_touch = false, rgrip_touch = false;

	// Analog (signed int16 unless noted)
	int16_t ltrigger = 0, rtrigger = 0;        // 0..32767
	int16_t lstick_x = 0, lstick_y = 0;        // -32768..+32767
	int16_t rstick_x = 0, rstick_y = 0;
	int16_t lpad_x = 0, lpad_y = 0;
	int16_t rpad_x = 0, rpad_y = 0;
	uint16_t lpad_pressure = 0, rpad_pressure = 0;

	// IMU
	int16_t accel_x = 0, accel_y = 0, accel_z = 0;
	int16_t gyro_x = 0, gyro_y = 0, gyro_z = 0;
};

// Parse a raw input report into SCState. Returns false if the report isn't a
// controller state packet or is too short.
bool parse_triton_report(const uint8_t* buf, size_t len, SCState& out);

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
