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
| SQM device library | Talk to SQM-LE (Ethernet) and SQM-LU (USB); parse the Unihedron protocol | Planned (M1) |
| `sqmctl` | CLI to query/configure a single SQM | Planned (M1) |
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
