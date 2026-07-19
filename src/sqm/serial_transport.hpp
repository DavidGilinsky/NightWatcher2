// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/serial_transport.hpp
// Purpose:       ITransport over a POSIX serial port for the SQM-LU (USB FTDI),
//                115200 8N1. Mirrors TcpTransport so the protocol codec and the
//                device layer are transport-agnostic.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

#include "transport.hpp"

namespace nightwatcher::sqm {

// Opens an SQM-LU over a serial device (e.g. "/dev/ttyUSB0" or a stable
// "/dev/serial/by-id/..." path) at 115200 8N1, no flow control. The constructor
// opens and configures the port (exclusive via TIOCEXCL) and throws
// std::runtime_error on failure. Reads are bounded by `timeout_ms` via poll().
class SerialTransport : public ITransport {
public:
    explicit SerialTransport(const std::string& device, int baud = 115200, int timeout_ms = 5000);
    ~SerialTransport() override;

    SerialTransport(const SerialTransport&) = delete;
    SerialTransport& operator=(const SerialTransport&) = delete;

    void write(const std::string& data) override;
    std::string read_line() override;

private:
    int fd_ = -1;
    int timeout_ms_ = 5000;
    std::string buf_;  // bytes read past a line boundary, kept for the next call
};

}  // namespace nightwatcher::sqm
