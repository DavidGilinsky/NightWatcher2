// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/sqm_device.cpp
// Purpose:       Implementation of SqmDevice (command -> parsed response).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "sqm_device.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace nightwatcher::sqm {

SqmDevice::SqmDevice(std::unique_ptr<ITransport> transport) : t_(std::move(transport)) {}

std::string SqmDevice::query_raw(const std::string& cmd) {
    t_->write(cmd);
    return t_->read_line();
}

Reading SqmDevice::read_averaged() { return parse_reading(query_raw(kCmdReading)); }
Reading SqmDevice::read_unaveraged() { return parse_reading(query_raw(kCmdUnaveraged)); }
Calibration SqmDevice::calibration() { return parse_calibration(query_raw(kCmdCalibration)); }
UnitInfo SqmDevice::info() { return parse_unit_info(query_raw(kCmdUnitInfo)); }

CalStatus SqmDevice::arm_light_calibration() { return parse_cal_status(query_raw(kCmdArmLight)); }
CalStatus SqmDevice::arm_dark_calibration() { return parse_cal_status(query_raw(kCmdArmDark)); }
CalStatus SqmDevice::disarm_calibration() { return parse_cal_status(query_raw(kCmdDisarm)); }

// Build "zcal<which><11-char zero-padded value>x" and confirm the echo.
CalSetEcho SqmDevice::set_cal_value(char which, double value, int decimals) {
    char cmd[48];
    std::snprintf(cmd, sizeof(cmd), "zcal%c%011.*fx", which, decimals, value);
    const CalSetEcho e = parse_cal_set_echo(query_raw(cmd));
    if (e.which != which) {
        throw std::runtime_error("unexpected calibration-set echo: '" + e.raw + "'");
    }
    return e;
}

CalSetEcho SqmDevice::set_light_offset(double mag_arcsec2) { return set_cal_value('5', mag_arcsec2, 2); }
CalSetEcho SqmDevice::set_light_temp(double celsius) { return set_cal_value('6', celsius, 2); }
CalSetEcho SqmDevice::set_dark_period(double seconds) { return set_cal_value('7', seconds, 3); }
CalSetEcho SqmDevice::set_dark_temp(double celsius) { return set_cal_value('8', celsius, 2); }

}  // namespace nightwatcher::sqm
