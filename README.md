<!--
  Author:        David Gilinsky
  File:          README.md
  Purpose:       Project overview, build instructions, and repository layout.
  Created:       2026-07-18
  Last Modified: 2026-07-18
  Version:       0.1.0
  License:       GPL-3.0-or-later
-->

# NightWatcher2

Tools to interface with Unihedron **Sky Quality Meters (SQMs)** in furtherance of
participating in the **[Dark Sky Network (DSN)](https://soazcomms.github.io/docs/DSN_Description.html)**,
operated by the Southern Arizona Dark Sky Association.

SQMs record sky brightness (magnitudes per square arc-second) every 5 minutes.
NightWatcher2 reads them, stores the readings in a time-series database, and exposes
that data through an API and a web UI.

## Components

| Component | Description | Status |
|-----------|-------------|--------|
| `nightwatcherd` | Daemon: polls SQM(s) at a configurable interval, records readings, serves the API | Skeleton (M0) |
| SQM device library | Talk to SQM-LE (Ethernet) and SQM-LU (USB); parse the Unihedron protocol; subnet discovery | SQM-LE + discovery done (M1); USB serial pending |
| `sqmctl` | CLI to discover and query a single SQM | Done (M1) |
| Database | MariaDB/MySQL store for readings + configuration/calibration history | Planned (M2) |
| REST API | Embedded HTTP server (JSON) to query and configure | Planned (M4) |
| Web UI | Status dashboard, configuration, query, time-series graph | Planned (M5) |

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the skeleton daemon:

```sh
./build/nightwatcherd --version
./build/nightwatcherd --config config/nightwatcher.conf.example
```

### Cross-compiling for ARM (e.g. Raspberry Pi 64-bit)

```sh
sudo apt install g++-aarch64-linux-gnu
cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DNW_BUILD_TESTS=OFF
cmake --build build-arm64 --parallel
```

## Talking to an SQM (`sqmctl`)

`sqmctl` finds SQM-LE units on the network and queries them over TCP.

### Finding a unit on the network

If the SQM-LE picked up an unknown IP address via DHCP, scan its subnet. Each host is
probed on port 10001 and confirmed with a unit-info (`ix`) query, so only real SQMs are
reported:

```sh
sqmctl discover 192.168.1.0/24
# Found 1 SQM(s):
#   192.168.1.73:10001  serial=00000413 model=3 feature=1 protocol=2
```

Options: `--port N` (default 10001), `--timeout MS` (default 700), `--concurrency N`
(default 128). A /24 scans in roughly a second.

### Querying a unit

```sh
sqmctl --tcp 192.168.1.50:10001 info    # unit info        (ix)
sqmctl --tcp 192.168.1.50:10001 read    # averaged reading (rx)
sqmctl --tcp 192.168.1.50 unaveraged    # unaveraged read  (ux)
sqmctl --tcp 192.168.1.50 cal           # calibration info (cx)
```

### Testing without hardware

A software simulator emits canned Unihedron responses so the library and CLI can be
exercised end-to-end without a physical meter:

```sh
./build/sqm-sim 10001 &                 # listen on 127.0.0.1:10001
./build/sqmctl --tcp 127.0.0.1:10001 read
```

## Configuration

Copy [`config/nightwatcher.conf.example`](config/nightwatcher.conf.example) to
`/etc/nightwatcher/nightwatcher.conf` and edit it. Each SQM is one `[sensor:<DSN-id>]`
block; `transport = tcp` targets an SQM-LE (default port 10001), `transport = serial`
targets an SQM-LU.

## Repository layout

```
src/sqm/      SQM device library (transports + protocol codec)
src/db/       database access layer
src/api/      embedded HTTP server + REST handlers
src/daemon/   nightwatcherd (scheduler, config, logging, main)
src/cli/      sqmctl command-line tool
include/      public headers
web/          static web UI
sql/          database schema + migrations
config/       example config + systemd unit
docs/         architecture, SQM protocol, DSN notes
tests/        unit tests
```

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — system architecture and roadmap
- [`docs/sqm-protocol.md`](docs/sqm-protocol.md) — Unihedron SQM command/response reference
- [`docs/dsn.md`](docs/dsn.md) — Dark Sky Network notes

## License

[GPL-3.0-or-later](LICENSE). The project may link GPL/LGPL libraries (e.g. MariaDB
Connector/C); the database client is MariaDB Connector/C (LGPL-2.1) rather than Oracle's
`libmysqlclient` for clean GPLv3 compatibility.
