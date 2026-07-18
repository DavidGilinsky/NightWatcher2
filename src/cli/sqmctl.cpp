// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/cli/sqmctl.cpp
// Purpose:       Command-line tool to query a single SQM over TCP (SQM-LE):
//                info (ix), read (rx), unaveraged (ux), cal (cx).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "nightwatcher/version.hpp"
#include "protocol.hpp"
#include "sqm_device.hpp"
#include "tcp_transport.hpp"

namespace {

using namespace nightwatcher::sqm;

void usage(const char* argv0) {
    std::cout
        << "sqmctl " << NIGHTWATCHER_VERSION << " \xe2\x80\x94 query a Unihedron SQM\n\n"
        << "Usage: " << argv0 << " --tcp HOST[:PORT] <command> [--timeout MS]\n\n"
        << "Commands:\n"
        << "  info         Unit information (ix)\n"
        << "  read         Averaged reading (rx)\n"
        << "  unaveraged   Unaveraged reading (ux)\n"
        << "  cal          Calibration information (cx)\n\n"
        << "The default port is 10001 (SQM-LE). --timeout defaults to 5000 ms.\n";
}

void print_reading(const Reading& r) {
    std::cout << "raw           : " << r.raw << "\n"
              << "mag/arcsec^2  : " << r.mag_arcsec2 << "\n"
              << "frequency Hz  : " << r.freq_hz << "\n"
              << "period counts : " << r.period_counts << "\n"
              << "period s      : " << r.period_s << "\n"
              << "temperature C : " << r.temp_c << "\n";
    if (!r.serial.empty()) std::cout << "serial        : " << r.serial << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string endpoint;
    std::string command;
    int timeout_ms = 5000;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--version") {
            std::cout << "sqmctl " << NIGHTWATCHER_VERSION << "\n";
            return 0;
        } else if (a == "--tcp") {
            if (++i >= argc) { std::cerr << "error: --tcp requires HOST[:PORT]\n"; return 2; }
            endpoint = argv[i];
        } else if (a == "--timeout") {
            if (++i >= argc) { std::cerr << "error: --timeout requires MS\n"; return 2; }
            timeout_ms = std::atoi(argv[i]);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option '" << a << "'\n";
            return 2;
        } else if (command.empty()) {
            command = a;
        } else {
            std::cerr << "error: unexpected argument '" << a << "'\n";
            return 2;
        }
    }

    if (endpoint.empty() || command.empty()) {
        usage(argv[0]);
        return 2;
    }

    // Parse HOST[:PORT]; default to the SQM-LE port 10001.
    std::string host = endpoint;
    uint16_t port = 10001;
    const auto colon = endpoint.rfind(':');
    if (colon != std::string::npos) {
        host = endpoint.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(endpoint.substr(colon + 1).c_str()));
    }

    try {
        auto transport = std::make_unique<TcpTransport>(host, port, timeout_ms);
        SqmDevice dev(std::move(transport));

        if (command == "info") {
            const UnitInfo u = dev.info();
            std::cout << "raw      : " << u.raw << "\n"
                      << "protocol : " << u.protocol << "\n"
                      << "model    : " << u.model << "\n"
                      << "feature  : " << u.feature << "\n"
                      << "serial   : " << u.serial << "\n";
        } else if (command == "read") {
            print_reading(dev.read_averaged());
        } else if (command == "unaveraged") {
            print_reading(dev.read_unaveraged());
        } else if (command == "cal") {
            const Calibration c = dev.calibration();
            std::cout << "raw               : " << c.raw << "\n"
                      << "light cal offset  : " << c.light_cal_offset << " mag\n"
                      << "dark cal period s : " << c.dark_cal_period_s << "\n"
                      << "temp @ light cal  : " << c.temp_light_c << " C\n"
                      << "sensor offset     : " << c.sensor_offset << " mag\n"
                      << "temp @ dark cal   : " << c.temp_dark_c << " C\n";
        } else {
            std::cerr << "error: unknown command '" << command << "'\n";
            usage(argv[0]);
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
