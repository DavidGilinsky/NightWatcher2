// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tools/sqm-sim.cpp
// Purpose:       Minimal TCP SQM simulator: emits canned Unihedron responses to
//                rx/ux/cx/ix so the device library and sqmctl can be tested
//                end-to-end without hardware. Binds to 127.0.0.1 only.
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
constexpr const char* kRespCx = "c,00000017.60m,0000000.000s, 039.4C,00000008.71m, 039.4C\r\n";

// Order matters: check "ix"/"ux" before their substrings can't collide with
// others. Commands are two chars, so a simple substring search suffices.
const char* response_for(const std::string& buf) {
    if (buf.find("ix") != std::string::npos) return kRespIx;
    if (buf.find("ux") != std::string::npos) return kRespUx;
    if (buf.find("cx") != std::string::npos) return kRespCx;
    if (buf.find("rx") != std::string::npos) return kRespRx;
    return nullptr;
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
            if (const char* resp = response_for(buf)) {
                ::send(cli, resp, std::strlen(resp), 0);
                buf.clear();
            }
        }
        ::close(cli);
    }
    ::close(srv);
    return 0;
}
