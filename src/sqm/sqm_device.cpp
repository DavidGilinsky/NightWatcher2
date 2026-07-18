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

}  // namespace nightwatcher::sqm
