// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/device_factory.hpp
// Purpose:       Build an SqmDevice from a stored (transport, address) pair so
//                the daemon, CLIs, and API don't each hard-code a transport.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>

#include "sqm_device.hpp"

namespace nightwatcher::sqm {

// Open an SQM for a stored sensor:
//   transport "tcp"    -> address "host[:port]" (default port 10001) via TcpTransport
//   transport "serial" -> address "/dev/tty..." (115200 8N1)         via SerialTransport
// Throws std::runtime_error on an unknown transport, or on a connect/open failure.
std::unique_ptr<SqmDevice> open_device(const std::string& transport, const std::string& address,
                                       int timeout_ms = 5000);

}  // namespace nightwatcher::sqm
