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

    // Send a raw command and return the raw response line (terminator stripped).
    std::string query_raw(const std::string& cmd);

private:
    std::unique_ptr<ITransport> t_;
};

}  // namespace nightwatcher::sqm
