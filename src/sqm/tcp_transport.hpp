// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/tcp_transport.hpp
// Purpose:       ITransport over TCP for the SQM-LE (Ethernet), default port
//                10001.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include "transport.hpp"

namespace nightwatcher::sqm {

// Connects to an SQM-LE over TCP. The constructor establishes the connection
// and applies a send/receive timeout; it throws std::runtime_error on failure.
class TcpTransport : public ITransport {
public:
    TcpTransport(const std::string& host, uint16_t port, int timeout_ms = 5000);
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    void write(const std::string& data) override;
    std::string read_line() override;

private:
    int fd_ = -1;
    std::string buf_;  // bytes read past a line boundary, kept for the next call
};

}  // namespace nightwatcher::sqm
