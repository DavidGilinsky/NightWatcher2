// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/protocol.hpp
// Purpose:       Unihedron SQM command strings and parsers for the reading (rx/
//                ux), calibration (cx), and unit-information (ix) responses.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace nightwatcher::sqm {

// Parsed 'rx'/'ux' reading response (see docs/sqm-protocol.md).
struct Reading {
    double mag_arcsec2 = 0.0;    // sky brightness (mag/arcsec^2)
    long long freq_hz = 0;       // sensor frequency (Hz)
    long long period_counts = 0; // sensor period (counts @ 460800 Hz)
    double period_s = 0.0;       // sensor period (seconds)
    double temp_c = 0.0;         // sensor temperature (deg C)
    std::string serial;          // present only in interval reports; else empty
    std::string raw;             // exact response line (terminator stripped)
};

// Parsed 'cx' calibration-information response.
struct Calibration {
    double light_cal_offset = 0.0;   // light calibration offset (mag/arcsec^2)
    double dark_cal_period_s = 0.0;  // dark calibration time period (s)
    double temp_light_c = 0.0;       // temperature during light calibration (C)
    double sensor_offset = 0.0;      // factory reference sensor offset (mag/arcsec^2)
    double temp_dark_c = 0.0;        // temperature during dark calibration (C)
    std::string raw;
};

// Parsed 'ix' unit-information response.
struct UnitInfo {
    int protocol = 0;    // protocol number
    int model = 0;       // model number
    int feature = 0;     // feature number
    std::string serial;  // 8-digit serial (kept as string to preserve leading zeros)
    std::string raw;
};

// Parsed arm/disarm calibration response, e.g. "zAaL" (light armed, locked),
// "zBaL" (dark armed), "zxdL" (all disarmed). The physical unlock switch, not
// software, triggers the actual measurement once a mode is armed.
struct CalStatus {
    char mode = 'x';     // 'A' = light, 'B' = dark, 'x' = all modes
    bool armed = false;  // true if 'a' (armed), false if 'd' (disarmed)
    bool locked = true;  // true if 'L' (unlock switch Locked), false if 'U' (Unlocked)
    std::string raw;
};

// Parsed echo of a manual calibration-set command, e.g. "z,5,00000017.60m":
// the device reports the value it actually wrote to EEPROM (which for temps may
// differ slightly from the requested value due to raw resolution).
struct CalSetEcho {
    char which = 0;      // '5' offset, '6' light temp, '7' dark period, '8' dark temp
    double value = 0.0;  // stored value with the trailing unit character stripped
    std::string raw;
};

// Command tokens sent to the device.
inline constexpr const char* kCmdReading = "rx";
inline constexpr const char* kCmdUnaveraged = "ux";
inline constexpr const char* kCmdCalibration = "cx";
inline constexpr const char* kCmdUnitInfo = "ix";
inline constexpr const char* kCmdArmLight = "zcalAx";  // arm light calibration
inline constexpr const char* kCmdArmDark = "zcalBx";   // arm dark calibration
inline constexpr const char* kCmdDisarm = "zcalDx";    // disarm; reports lock status

// Parsers. Each throws std::runtime_error if the line is not the expected type
// or a field cannot be parsed.
Reading parse_reading(const std::string& line);
Calibration parse_calibration(const std::string& line);
UnitInfo parse_unit_info(const std::string& line);
CalStatus parse_cal_status(const std::string& line);
CalSetEcho parse_cal_set_echo(const std::string& line);

}  // namespace nightwatcher::sqm
