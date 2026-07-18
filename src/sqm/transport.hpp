// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/transport.hpp
// Purpose:       Abstract byte-stream transport for talking to an SQM (TCP or
//                serial); the protocol codec is transport-agnostic.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace nightwatcher::sqm {

// A bidirectional byte-stream connection to a single SQM.
// Implementations: TcpTransport (SQM-LE) and, later, SerialTransport (SQM-LU).
class ITransport {
public:
    virtual ~ITransport() = default;

    // Send raw bytes to the device (e.g. the command "rx").
    virtual void write(const std::string& data) = 0;

    // Read one response line terminated by CR/LF or LF; the terminator is
    // stripped. Throws std::runtime_error on timeout, EOF, or I/O error.
    virtual std::string read_line() = 0;
};

}  // namespace nightwatcher::sqm
