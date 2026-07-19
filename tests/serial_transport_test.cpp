// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/serial_transport_test.cpp
// Purpose:       Unit test for SerialTransport and open_device using a
//                pseudo-terminal as a stand-in SQM-LU: a canned ix response is
//                read + parsed, a silent port times out, and the factory
//                rejects unknown transports. No real hardware required.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "device_factory.hpp"
#include "protocol.hpp"
#include "serial_transport.hpp"
#include "sqm_device.hpp"

using namespace nightwatcher::sqm;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// Open a pseudo-terminal; return the slave device path (empty on failure) and
// hand back the master fd, which we drive as the fake SQM.
static std::string open_pty(int* master) {
    const int m = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return "";
    if (::grantpt(m) != 0 || ::unlockpt(m) != 0) { ::close(m); return ""; }
    const char* name = ::ptsname(m);
    if (name == nullptr) { ::close(m); return ""; }
    *master = m;
    return name;
}

int main() {
    // 1) SerialTransport reads a real ix response over the port; SqmDevice parses it.
    {
        int master = -1;
        const std::string slave = open_pty(&master);
        if (slave.empty()) { std::puts("serial_transport_test skipped (no pty available)"); return 0; }
        auto t = std::make_unique<SerialTransport>(slave, 115200, 1000);
        // Queue the response after the constructor's flush, before the query.
        const char* resp = "i,00000002,00000003,00000001,00000413\r\n";
        CHECK(::write(master, resp, std::strlen(resp)) > 0);
        SqmDevice dev(std::move(t));
        try {
            const UnitInfo u = dev.info();  // sends ix on the slave, reads the queued reply
            CHECK(u.protocol == 2);
            CHECK(u.model == 3);
            CHECK(u.feature == 1);
            CHECK(u.serial == "00000413");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "info() threw: %s\n", e.what());
            ++g_failures;
        }
        ::close(master);
    }

    // 2) A silent port times out (bounded by poll()).
    {
        int master = -1;
        const std::string slave = open_pty(&master);
        if (!slave.empty()) {
            SerialTransport t(slave, 115200, 150);
            try {
                t.read_line();
                CHECK(false);  // should not return
            } catch (const std::exception& e) {
                CHECK(std::string(e.what()).find("timeout") != std::string::npos);
            }
            ::close(master);
        }
    }

    // 3) The factory rejects an unknown transport.
    try {
        (void)open_device("carrier-pigeon", "nowhere", 100);
        CHECK(false);
    } catch (const std::exception&) {
        // expected
    }

    if (g_failures == 0) {
        std::puts("serial_transport_test passed");
        return 0;
    }
    std::fprintf(stderr, "serial_transport_test FAILED (%d checks)\n", g_failures);
    return 1;
}
