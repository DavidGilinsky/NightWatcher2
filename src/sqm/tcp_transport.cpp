// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/tcp_transport.cpp
// Purpose:       POSIX-sockets implementation of TcpTransport (SQM-LE). Uses a
//                non-blocking connect bounded by a poll() timeout so unreachable
//                hosts fail fast (important for subnet discovery).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "tcp_transport.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace nightwatcher::sqm {
namespace {

// Connect `fd` to `addr` but give up after `timeout_ms`. Returns true on a
// completed connection. Leaves the socket in blocking mode on return.
bool connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t len,
                          int timeout_ms) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    bool ok = false;
    if (::connect(fd, addr, len) == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int pr;
        do {
            pr = ::poll(&pfd, 1, timeout_ms);
        } while (pr < 0 && errno == EINTR);
        if (pr > 0 && (pfd.revents & POLLOUT)) {
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr == 0) {
                ok = true;
            }
        }
    }

    ::fcntl(fd, F_SETFL, flags);  // restore blocking mode
    return ok;
}

}  // namespace

TcpTransport::TcpTransport(const std::string& host, uint16_t port, int timeout_ms) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string port_s = std::to_string(port);
    struct addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo(" + host + "): " + gai_strerror(rc));
    }

    int fd = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        if (connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen, timeout_ms)) {
            // Apply send/receive timeouts for the subsequent command exchange.
            struct timeval tv {};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);

    if (fd < 0) {
        throw std::runtime_error("cannot connect to " + host + ":" + port_s + ": " +
                                 std::strerror(errno));
    }
    fd_ = fd;
}

TcpTransport::~TcpTransport() {
    if (fd_ >= 0) ::close(fd_);
}

void TcpTransport::write(const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("send: ") + std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

std::string TcpTransport::read_line() {
    for (;;) {
        const auto nl = buf_.find('\n');
        if (nl != std::string::npos) {
            std::string line = buf_.substr(0, nl);
            buf_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }

        char chunk[256];
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) {
            throw std::runtime_error("connection closed by peer");
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw std::runtime_error("read timeout");
            }
            throw std::runtime_error(std::string("recv: ") + std::strerror(errno));
        }
        buf_.append(chunk, static_cast<size_t>(n));
    }
}

}  // namespace nightwatcher::sqm
