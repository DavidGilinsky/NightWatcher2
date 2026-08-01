<!--
  Author:        David Gilinsky
  File:          docs/architecture.md
  Purpose:       System architecture, components, technology choices, and roadmap.
  Created:       2026-07-18
  Last Modified: 2026-08-01
  Version:       0.1.2
  License:       GPL-3.0-or-later
-->

# NightWatcher2 architecture

## Overview

A single Linux daemon, `nightwatcherd` (amd64 and arm64), polls one or more Unihedron
SQMs at a configurable interval, records readings into a MariaDB/MySQL time-series
database, and serves both a REST API and a static web UI over an embedded HTTPS server.
Around that core it also polls a co-located weather station, runs scheduled data exports,
authenticates users, and hosts read-only status tabs for companion tools.

```
   SQM-LE (TCP:10001) ─┐   ┌───────────────── nightwatcherd ──────────────────┐
   SQM-LU (USB serial) ┼──▶│ SQM device library → scheduler ──┐                │
                       │   │                                  ▼                │──▶ MariaDB
   weather station ────────│ weather scheduler ───────────▶ DB layer ◀── export│    (readings,
   (WU / Ambient API) ─────│                                  │       scheduler │     weather,
                       │   │   embedded HTTPS server (cpp-httplib + OpenSSL)     │     users,
                       │   │     • REST API (JSON)  • auth/sessions  • web UI    │     exports,
                       │   └─────────┬──────────────────┬───────────────────────┘     extensions…)
   sqmctl / nwdb CLI ──┘            │                    │                        │
                     Browser (web UI) ◀─┐         DSN export ──▶ Google Drive     │
                     API clients ◀──────┴─┐       webhook export ──▶ WordPress ◀──┘
                     companion tools (extensions tab) ─┘
```

## Components and technology

- **SQM device library** (`src/sqm/`) — an `ITransport` abstraction with `TcpTransport`
  (SQM-LE) and `SerialTransport` (SQM-LU), plus a transport-agnostic `Protocol` codec that
  parses the fixed-column responses documented in [`sqm-protocol.md`](sqm-protocol.md).
  `discovery` finds SQM-LE units by CIDR subnet scan and SQM-LU units by probing the local
  serial/USB bus.
- **Database layer** (`src/db/`) — MariaDB Connector/C (`libmariadb`, LGPL-2.1) wrapped in
  an RAII C++ class. Chosen over Oracle `libmysqlclient` (GPLv2) for clean GPL-3.0 linking.
  The database is the source of truth for the sensor list, weather stations, users, and
  export targets.
- **Daemon** (`src/daemon/`) — config parsing, the sensor `Scheduler` (polls each active
  sensor on its own `poll_interval_s`, default 300), the weather scheduler, the export
  scheduler, logging, `sigwait` shutdown / `SIGHUP` reload, and systemd integration.
- **API** (`src/api/`) — cpp-httplib (MIT) embedded server with self-signed TLS
  (`tls_cert`, generated at first start into the systemd state directory); JSON via
  nlohmann/json (MIT). Endpoint families: `sensors`, `readings` under sensors, `events`,
  `settings`, `users`, `login`/`logout`/`me`, `discover` (+ `discover/usb`), `extensions`,
  `db` (init), `health`, `version`, and `platform`.
- **Authentication** (`src/auth/`) — PBKDF2 password hashing with server-side sessions and
  two roles (`admin`, `viewer`). The daemon seeds an `admin` account on first start. Writes
  require a login; reads are open on localhost.
- **Weather integration** (`src/weather/`) — a provider interface with two implementations,
  Weather Underground and Ambient Weather Network, over an HTTPS client (httplib + OpenSSL).
  The weather scheduler polls the configured station into `weather_readings`.
- **Export** (`src/export/`) — a scheduled export engine with two shapes: *monthly* exporters
  rebuild each affected local-month in full (the **DSN** community `.dat` format, delivered
  to a Google Drive folder or a local outbox), and *incremental* exporters push only readings
  newer than a watermark (the **webhook** target that feeds the WordPress plugin). Includes
  `dsn_format`, `gdrive`, `export_runner`, and `export_scheduler`.
- **Web UI** (`web/`) — static HTML/CSS/vanilla-JS, no build step and no external CDNs. The
  time-series graph uses uPlot (MIT); sun and moon altitude and an optional co-located
  ambient-temperature series are overlaid via SunCalc (BSD), all vendored.
- **Platform proxy** (`src/api/`, `/api/v1/platform/*`) — where a helper's unix socket
  (`/run/nightwatcher-platform.sock`) exists, admin-authenticated platform routes proxy to
  it; everywhere else they 404. This is the seam the NightWatcher-Pi appliance uses to add an
  RPi tab without putting any Raspberry Pi knowledge into the core.

## Data model

MariaDB tables (`sql/schema.sql`), self-created on first start: `sensors`, `readings`,
`config_log`, `events`, `weather_stations`, `weather_readings`, `users`, `sessions`,
`export_targets`, `export_log`, `settings`, and `extensions` (the registry backing the
read-only companion-tool tabs).

## Language and build

C++17 for the core with a C-friendly device layer; CMake build; third-party headers
(`httplib`, `nlohmann`) vendored under `third_party/`. Linux amd64 and arm64 (Raspberry Pi
is a first-class target). Packaged as a `.deb` via CPack with debconf-driven setup and a
systemd unit. CI builds and tests on amd64 (against a MariaDB service), cross-compiles for
arm64, and builds/installs/verifies the `.deb` for both. License: GPL-3.0-or-later.

## Roadmap

- **M0 — Bootstrap** *(done)*: repo, CMake scaffold, skeleton daemon, CI, docs, schema.
- **M1 — SQM library + `sqmctl`** *(done)*: TCP (SQM-LE) and serial (SQM-LU) transports,
  protocol codec, simulator, discovery, tests.
- **M2 — Database layer** *(done)*: `libmariadb` wrapper, schema, and the `nwdb` CLI.
- **M3 — Daemon** *(done)*: per-sensor `Scheduler` → DB, `events` logging, `sigwait`
  shutdown / `SIGHUP` reload, systemd.
- **M4 — API** *(done)*: cpp-httplib + REST endpoints, self-signed TLS, PBKDF2 auth and
  sessions, the extensions registry, and the platform proxy.
- **M5 — Web UI** *(done)*: dashboard, config, query, and the time-series graph with
  sun/moon and ambient overlays.
- **M6 — Packaging & DSN export** *(done)*: CPack `.deb` for amd64/arm64; the DSN monthly
  export to Google Drive and the incremental WordPress webhook export.
- **Weather integration** *(done)*: Weather Underground and Ambient Weather Network polled
  into `weather_readings` and overlaid on the graph.

Companion projects live in their own repositories: **NightWatcher-Pi** bundles this daemon
into a turnkey Raspberry Pi access-point image (via the platform proxy above), and
**NightWatcher-AirWatcher** registers a read-only tab through the extensions registry. This
file is the in-repo architecture summary, kept current as the project evolves.
