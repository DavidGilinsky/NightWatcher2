// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/serial_transport.cpp
// Purpose:       POSIX termios implementation of SerialTransport (SQM-LU). Opens
//                the port 8N1 at the given baud (default 115200), raw, no flow
//                control, exclusive, with poll()-bounded reads so a silent
//                device fails fast (important for USB discovery).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "serial_transport.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace nightwatcher::sqm {
namespace {

// Map a numeric baud rate to its termios speed constant (defaults to B115200,
// the SQM-LU's rate, for anything unrecognised).
speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default: return B115200;
    }
}

}  // namespace

SerialTransport::SerialTransport(const std::string& device, int baud, int timeout_ms)
    : timeout_ms_(timeout_ms) {
    const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        throw std::runtime_error("open(" + device + "): " + std::strerror(errno));
    }
    // Claim the port exclusively so a routine poll and a manual test/discover
    // can't garble each other by opening the same device at once.
    ::ioctl(fd, TIOCEXCL);

    struct termios tio {};
    if (::tcgetattr(fd, &tio) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::runtime_error("tcgetattr(" + device + "): " + std::strerror(e));
    }
    ::cfmakeraw(&tio);  // no echo/canonical/signal processing, 8-bit clean
    const speed_t sp = baud_to_speed(baud);
    ::cfsetispeed(&tio, sp);
    ::cfsetospeed(&tio, sp);
    tio.c_cflag |= (CLOCAL | CREAD);              // ignore modem lines, enable receiver
    tio.c_cflag &= ~CSTOPB;                        // 1 stop bit
    tio.c_cflag &= ~PARENB;                        // no parity
    tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;    // 8 data bits
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;                        // no hardware flow control
#endif
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);        // no software flow control
    tio.c_cc[VMIN] = 0;                            // non-blocking; read_line() polls
    tio.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::runtime_error("tcsetattr(" + device + "): " + std::strerror(e));
    }
    ::tcflush(fd, TCIOFLUSH);  // discard any boot/garbage bytes already buffered
    fd_ = fd;
}

SerialTransport::~SerialTransport() {
    if (fd_ >= 0) ::close(fd_);
}

void SerialTransport::write(const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::write(fd_, data.data() + sent, data.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd {};
                pfd.fd = fd_;
                pfd.events = POLLOUT;
                ::poll(&pfd, 1, timeout_ms_);
                continue;
            }
            throw std::runtime_error(std::string("serial write: ") + std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

std::string SerialTransport::read_line() {
    for (;;) {
        const auto nl = buf_.find('\n');
        if (nl != std::string::npos) {
            std::string line = buf_.substr(0, nl);
            buf_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }

        struct pollfd pfd {};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int pr;
        do {
            pr = ::poll(&pfd, 1, timeout_ms_);
        } while (pr < 0 && errno == EINTR);
        if (pr == 0) throw std::runtime_error("read timeout");
        if (pr < 0) throw std::runtime_error(std::string("poll: ") + std::strerror(errno));

        char chunk[256];
        const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
        if (n == 0) throw std::runtime_error("serial closed");
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::runtime_error(std::string("serial read: ") + std::strerror(errno));
        }
        buf_.append(chunk, static_cast<size_t>(n));
    }
}

}  // namespace nightwatcher::sqm
