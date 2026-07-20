// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tools/sqm-sim.cpp
// Purpose:       Minimal TCP SQM simulator: emits Unihedron responses to
//                rx/ux/cx/ix and the calibration commands (zcalA/B/D, zcal5-8)
//                so the device library, sqmctl, and the daemon can be tested
//                end-to-end without hardware. Calibration state is mutable, so
//                a manual set is reflected in the next cx. Binds to 127.0.0.1.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// Example responses from the Unihedron SQM-LU manual (see docs/sqm-protocol.md).
constexpr const char* kRespIx = "i,00000002,00000003,00000001,00000413\r\n";
constexpr const char* kRespRx = "r, 06.70m,0000022921Hz,0000000020c,0000000.000s, 039.4C\r\n";
constexpr const char* kRespUx = "u, 06.53m,0000022921Hz,0000000020c,0000000.000s, 039.4C\r\n";

// Mutable calibration, so manual sets (zcal5-8) are reflected in the next cx.
struct SimState {
    double light_offset = 17.60;  // mag/arcsec^2
    double dark_period = 0.0;     // s
    double temp_light = 39.4;     // C
    double sensor_offset = 8.71;  // mag/arcsec^2 (factory)
    double temp_dark = 39.4;      // C
    char armed = 0;               // 'A' light, 'B' dark, 0 none
    bool locked = true;           // unlock switch state (L/U)
};

// Extract the numeric value of a "zcal<N>...x" command (between prefix and 'x').
double set_value(const std::string& buf, size_t prefix_end, bool* complete) {
    const auto x = buf.find('x', prefix_end);
    *complete = (x != std::string::npos);
    if (!*complete) return 0.0;
    return std::atof(buf.substr(prefix_end, x - prefix_end).c_str());
}

std::string cx_line(const SimState& st) {
    char b[128];
    std::snprintf(b, sizeof(b), "c,%011.2fm,%011.3fs,% 06.1fC,%011.2fm,% 06.1fC\r\n",
                  st.light_offset, st.dark_period, st.temp_light, st.sensor_offset, st.temp_dark);
    return b;
}

// Returns the response for a complete command, or "" if none/incomplete.
std::string response_for(const std::string& buf, SimState& st) {
    size_t p;
    if ((p = buf.find("zcalA")) != std::string::npos) { st.armed = 'A'; return std::string("zAa") + (st.locked ? 'L' : 'U') + "\r\n"; }
    if ((p = buf.find("zcalB")) != std::string::npos) { st.armed = 'B'; return std::string("zBa") + (st.locked ? 'L' : 'U') + "\r\n"; }
    if ((p = buf.find("zcalD")) != std::string::npos) { st.armed = 0;   return std::string("zxd") + (st.locked ? 'L' : 'U') + "\r\n"; }
    for (const char which : {'5', '6', '7', '8'}) {
        const std::string tok = std::string("zcal") + which;
        if ((p = buf.find(tok)) == std::string::npos) continue;
        bool complete = false;
        const double v = set_value(buf, p + tok.size(), &complete);
        if (!complete) return "";  // wait for the trailing 'x'
        char b[64];
        if (which == '5') { st.light_offset = v; std::snprintf(b, sizeof(b), "z,5,%011.2fm\r\n", v); }
        else if (which == '6') { st.temp_light = v; std::snprintf(b, sizeof(b), "z,6,%05.1fC\r\n", v); }
        else if (which == '7') { st.dark_period = v; std::snprintf(b, sizeof(b), "z,7,%011.3fs\r\n", v); }
        else { st.temp_dark = v; std::snprintf(b, sizeof(b), "z,8,%05.1fC\r\n", v); }
        return b;
    }
    if (buf.find("ix") != std::string::npos) return kRespIx;
    if (buf.find("ux") != std::string::npos) return kRespUx;
    if (buf.find("cx") != std::string::npos) return cx_line(st);
    if (buf.find("rx") != std::string::npos) return kRespRx;
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 10001;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::perror("socket"); return 1; }

    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }
    if (::listen(srv, 4) < 0) { std::perror("listen"); return 1; }
    std::fprintf(stderr, "sqm-sim listening on 127.0.0.1:%u\n", static_cast<unsigned>(port));

    SimState st;  // persists across connections, like the unit's EEPROM
    for (;;) {
        const int cli = ::accept(srv, nullptr, nullptr);
        if (cli < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            break;
        }
        std::string buf;
        char chunk[128];
        for (;;) {
            const ssize_t n = ::recv(cli, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.append(chunk, static_cast<size_t>(n));
            const std::string resp = response_for(buf, st);
            if (!resp.empty()) {
                ::send(cli, resp.data(), resp.size(), 0);
                buf.clear();
            }
        }
        ::close(cli);
    }
    ::close(srv);
    return 0;
}
