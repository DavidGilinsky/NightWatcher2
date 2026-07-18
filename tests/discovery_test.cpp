// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/discovery_test.cpp
// Purpose:       Unit tests for CIDR expansion (cidr_hosts). Network scanning
//                itself is exercised end-to-end against sqm-sim, not here.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "discovery.hpp"

using namespace nightwatcher::sqm;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n",         \
                         __LINE__, #cond);                                \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    // /24 excludes network and broadcast: 254 usable hosts.
    {
        const auto h = cidr_hosts("192.168.1.0/24");
        CHECK(h.size() == 254);
        CHECK(h.front() == "192.168.1.1");
        CHECK(h.back() == "192.168.1.254");
    }

    // A host address inside the block still resolves to the same range.
    {
        const auto h = cidr_hosts("192.168.1.50/24");
        CHECK(h.size() == 254);
        CHECK(h.front() == "192.168.1.1");
    }

    // /30 => 2 usable hosts.
    {
        const auto h = cidr_hosts("192.168.1.8/30");
        CHECK(h.size() == 2);
        CHECK(h[0] == "192.168.1.9");
        CHECK(h[1] == "192.168.1.10");
    }

    // /32 => the single host, inclusive.
    {
        const auto h = cidr_hosts("10.0.0.5/32");
        CHECK(h.size() == 1);
        CHECK(h[0] == "10.0.0.5");
    }

    // /31 => both addresses, inclusive.
    {
        const auto h = cidr_hosts("10.0.0.4/31");
        CHECK(h.size() == 2);
        CHECK(h[0] == "10.0.0.4");
        CHECK(h[1] == "10.0.0.5");
    }

    // Malformed inputs must throw.
    {
        bool threw = false;
        try { cidr_hosts("192.168.1.0"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }
    {
        bool threw = false;
        try { cidr_hosts("not-an-ip/24"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }

    if (g_failures == 0) {
        std::puts("discovery_test passed");
        return 0;
    }
    std::fprintf(stderr, "discovery_test FAILED (%d checks)\n", g_failures);
    return 1;
}
