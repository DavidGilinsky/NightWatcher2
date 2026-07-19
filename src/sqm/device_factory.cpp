// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/device_factory.cpp
// Purpose:       Implementation of open_device: pick TcpTransport or
//                SerialTransport from the transport string and wrap it in an
//                SqmDevice.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "device_factory.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include "serial_transport.hpp"
#include "tcp_transport.hpp"

namespace nightwatcher::sqm {

std::unique_ptr<SqmDevice> open_device(const std::string& transport, const std::string& address,
                                       int timeout_ms) {
    std::unique_ptr<ITransport> t;
    if (transport == "tcp") {
        std::string host = address;
        uint16_t port = 10001;  // Lantronix XPort default
        const auto colon = address.rfind(':');
        if (colon != std::string::npos) {
            host = address.substr(0, colon);
            port = static_cast<uint16_t>(std::atoi(address.substr(colon + 1).c_str()));
        }
        t = std::make_unique<TcpTransport>(host, port, timeout_ms);
    } else if (transport == "serial") {
        t = std::make_unique<SerialTransport>(address, 115200, timeout_ms);
    } else {
        throw std::runtime_error("unsupported transport '" + transport + "' (use tcp or serial)");
    }
    return std::make_unique<SqmDevice>(std::move(t));
}

}  // namespace nightwatcher::sqm
