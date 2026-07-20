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

SQMs measure sky brightness (magnitude per square arc-second).
NightWatcher2 reads them, stores the readings in a time-series database, and exposes
that data through an API and a web UI. It can also automatically upload data to the

## Components

| Component | Description | Status |
|-----------|-------------|--------|
| `nightwatcherd` | Daemon: polls active DB sensors on their interval, records readings, logs events | Polling done (M3); API pending |
| SQM device library | Talk to SQM-LE (Ethernet) and SQM-LU (USB); parse the Unihedron protocol; subnet discovery | SQM-LE + discovery done (M1); USB serial pending |
| `sqmctl` | CLI to discover and query a single SQM | Done (M1) |
| Database | MariaDB store (libmariadb) for readings + configuration/calibration history | Done (M2) |
| `nwdb` | CLI to register sensors, poll an SQM into the database, and query readings | Done (M2) |
| REST API | Embedded HTTP server (JSON): CRUD, query, DB setup/maintenance, live poll/discover | Core done (M4) |
| Web UI | Login, status dashboard, sensor/weather config, query, time-series graph, users, DB maintenance | Done (M5) |

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16, and `libmariadb-dev` for the database layer
(or configure with `-DNW_WITH_DB=OFF` to skip it).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the skeleton daemon:

```sh
./build/nightwatcherd --version
./build/nightwatcherd --config build/nightwatcher.conf.example
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

## Database

Readings and configuration are stored in MariaDB via MariaDB Connector/C (`libmariadb`).

The schema ([sql/schema.sql](sql/schema.sql)) has six tables: `sensors`, `readings` (with a
`quality` flag for saturated/suspect data), `config_log`, an operational `events` log, and
`weather_stations` / `weather_readings` for a co-located weather station such as an Ambient
Weather WS-2000 (polling is a future module). Stored units are metric/SI.

### One-time setup

```sh
sudo apt install -y mariadb-server libmariadb-dev
sudo mariadb < sql/setup.sql     # create the database + application user
sudo mariadb < sql/schema.sql    # create the tables
```

`setup.sql` creates a development user `nightwatcher` with password `nightwatcher` — change
it for any real deployment. The tools read the connection from the environment:

```sh
export NW_DB_HOST=127.0.0.1 NW_DB_PORT=3306
export NW_DB_USER=nightwatcher NW_DB_PASSWORD=nightwatcher NW_DB_NAME=nightwatcher
```

### `nwdb`

Register a sensor with full site metadata (serial/protocol/feature are auto-filled from the
device via `ix` unless `--no-probe`):

```sh
nwdb add-sensor DSN003 --tcp 172.22.4.112:10001 \
     --name Sugarloaf --site "Sugarloaf Peak" \
     --lat 31.9500 --lon -111.6000 --elev 1200 \
     --timezone America/Phoenix --installed 2026-07-18
nwdb set-sensor DSN003 --elev 1205    # partial edit — only elevation changes
nwdb show DSN003                      # full metadata for one sensor
nwdb sensors                          # list registered sensors
nwdb poll DSN003                      # read the SQM now and store the reading
nwdb readings DSN003                  # show recent stored readings
nwdb cal DSN003                       # read + store calibration
```

## API

`nightwatcherd` serves a JSON API on `[api] bind:port` (default `127.0.0.1:8080`), sharing
the same `nw_db` layer as `nwdb` — so the web UI drives the same code. **Reads are open**
(sensor data is public); **writes require an `admin` login session** or the optional static
`NW_API_TOKEN`. On first start the daemon seeds an `admin` account (password `admin`, flagged
must-change).

Log in (cookie-based sessions), then use the cookie — or the token the login returns — for writes:

```sh
curl -s -c jar -X POST localhost:8080/api/v1/login -d '{"username":"admin","password":"admin"}'
curl -s -b jar localhost:8080/api/v1/me
curl -s -b jar -X POST localhost:8080/api/v1/me/password -d '{"current_password":"admin","new_password":"s3cret"}'
curl -s -b jar -X POST localhost:8080/api/v1/users -d '{"username":"obs","password":"pw","role":"viewer"}'
```

Roles: `admin` (full access + user management) and `viewer` (read-only). Alternatively, a static
token for scripts:

```sh
export NW_API_TOKEN=$(openssl rand -hex 16)
curl -s localhost:8080/api/v1/health
curl -s localhost:8080/api/v1/sensors
curl -s "localhost:8080/api/v1/sensors/DSN003/readings?limit=10"
curl -s "localhost:8080/api/v1/discover?cidr=172.22.4.0/24"

AUTH="Authorization: Bearer $NW_API_TOKEN"
curl -s -X POST  localhost:8080/api/v1/sensors -H "$AUTH" \
  -d '{"id":"DSN003","tcp":"172.22.4.112:10001","name":"Sugarloaf","lat":31.9500,"lon":-111.6000,"elev":1200}'
curl -s -X PATCH localhost:8080/api/v1/sensors/DSN003 -H "$AUTH" -d '{"elevation_m":1205}'
curl -s -X POST  localhost:8080/api/v1/sensors/DSN003/poll -H "$AUTH"
curl -s -X POST  localhost:8080/api/v1/db/init -H "$AUTH"     # create any missing tables
```

Also: weather-station CRUD (`/api/v1/weather-stations`), `GET /db/status`, and
`DELETE /sensors/{id}/readings?before=DATE` (pruning).

## Installing

NightWatcher installs as a **self-contained bundle** under a single prefix (default
`/usr/local/nightwatcher`), which keeps the install tidy and trivial to remove:

```sh
cmake --build build --parallel
sudo cmake --install build          # or: sudo make -C build install
```

This lays down `bin/`, `web/`, `sql/`, `share/`, and the systemd unit under the prefix, then
(reaching outside it) symlinks the CLIs onto `PATH` (`/usr/local/bin/{sqmctl,nwdb,nwexport-auth}`),
symlinks the unit into `/etc/systemd/system/`, and seeds `/etc/nightwatcher/nightwatcher.conf`
(never overwriting an existing one). Change the prefix with `-DCMAKE_INSTALL_PREFIX=…`.

To finish: `sudo systemctl daemon-reload && sudo systemctl enable --now nightwatcherd`.
To remove everything: `sudo /usr/local/nightwatcher/uninstall.sh` (or `sudo make -C build uninstall`) —
this leaves `/etc/nightwatcher` (your config + secrets) and the database in place.

### Debian package (`.deb`)

CI builds installable `.deb`s for **amd64** and **arm64** (Raspberry Pi) on every push — download
them from the run's *Artifacts*. To build one yourself:

```sh
cmake -B pkg -DNW_INSTALL_SYSTEM_LINKS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build pkg --parallel
( cd pkg && cpack -G DEB )                 # -> pkg/nightwatcher_<ver>_<arch>.deb
sudo apt install ./pkg/nightwatcher_*.deb  # resolves libmariadb3/libssl3, runs the postinst
```

The package installs the bundle under `/usr/local/nightwatcher`; its **postinst** creates the
PATH/systemd/udev symlinks, seeds `/etc/nightwatcher/nightwatcher.conf`, installs the udev rule for
USB SQM-LU access, adds the installing user to `dialout`, and enables the service. `apt remove`
reverses it; `apt purge` also drops `/etc/nightwatcher`.

## Configuration

`make install` seeds `/etc/nightwatcher/nightwatcher.conf` from
[`config/nightwatcher.conf.example.in`](config/nightwatcher.conf.example.in); edit it for your
site. It configures only the daemon itself (database connection + API); **sensors are registered
in the database** with `nwdb add-sensor` (or the API), and each sensor's cadence is its
`poll_interval_s`.

## Web UI

With `web_root` set (default `/usr/local/nightwatcher/web`), the daemon serves a browser UI at
`http://<host>:<api-port>/` — static HTML/JS (dark theme, uPlot graph, no external CDNs) talking to
the API. It provides a login page (default `admin`/`admin`, must-change on first login), a live
status **dashboard**, **sensor** and **weather-station** management, a readings **query with a
time-series graph**, an **events** log, **user** management, and **database** maintenance
(schema status/init, pruning). Admin-only controls are hidden for `viewer` accounts.

## Running the daemon

`nightwatcherd` polls every `status = 'active'` sensor from the database on its interval,
stores readings (with the `quality` flag), and logs connect/disconnect/errors to the
`events` table.

```sh
export NW_DB_PASSWORD=nightwatcher
./build/nightwatcherd --config build/nightwatcher.conf.example
```

Signals: `SIGTERM` / `SIGINT` shut down gracefully; `SIGHUP` reloads the sensor list (so a
sensor added via `nwdb` is picked up without a restart). Under systemd (the unit is installed and
enabled as above, from [`config/systemd/nightwatcherd.service.in`](config/systemd/nightwatcherd.service.in)),
put the database password in `/etc/nightwatcher/nightwatcher.env` as `NW_DB_PASSWORD=...` (mode
`0600`). The unit runs as a `DynamicUser` in the `dialout` group so it can reach a USB SQM-LU on
`/dev/ttyUSB*`.

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
