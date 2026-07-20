// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/sqm_device.hpp
// Purpose:       High-level SQM device: issues commands over an ITransport and
//                returns parsed Reading/Calibration/UnitInfo results.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>

#include "protocol.hpp"
#include "transport.hpp"

namespace nightwatcher::sqm {

// Owns a transport and exposes the common SQM queries.
class SqmDevice {
public:
    explicit SqmDevice(std::unique_ptr<ITransport> transport);

    Reading read_averaged();    // rx
    Reading read_unaveraged();  // ux
    Calibration calibration();  // cx
    UnitInfo info();            // ix

    // Calibration control. Arming enables a mode; the operator then flips the
    // physical unlock switch to trigger the measurement. Disarm reports the
    // lock-switch status. These change device state, not the caller's.
    CalStatus arm_light_calibration();   // zcalAx
    CalStatus arm_dark_calibration();    // zcalBx
    CalStatus disarm_calibration();      // zcalDx

    // Manually write a calibration value to EEPROM (restore/copy calibration).
    // Each returns the value the device reports storing. These modify the unit.
    CalSetEcho set_light_offset(double mag_arcsec2);  // zcal5
    CalSetEcho set_light_temp(double celsius);        // zcal6
    CalSetEcho set_dark_period(double seconds);       // zcal7
    CalSetEcho set_dark_temp(double celsius);         // zcal8

    // Send a raw command and return the raw response line (terminator stripped).
    std::string query_raw(const std::string& cmd);

private:
    CalSetEcho set_cal_value(char which, double value, int decimals);

    std::unique_ptr<ITransport> t_;
};

}  // namespace nightwatcher::sqm
