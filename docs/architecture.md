<!--
  Author:        David Gilinsky
  File:          docs/architecture.md
  Purpose:       System architecture, components, technology choices, and roadmap.
  Created:       2026-07-18
  Last Modified: 2026-07-18
  Version:       0.1.0
  License:       GPL-3.0-or-later
-->

# NightWatcher2 architecture

## Overview

A single Linux daemon, `nightwatcherd` (x86 and ARM), polls one or more Unihedron SQMs at
a configurable interval, records readings into a MariaDB/MySQL time-series database, and
exposes both a REST API and a static web UI through an embedded HTTP server.

```
   SQM-LE (TCP:10001) ─┐   ┌──────────────── nightwatcherd ────────────────┐
   SQM-LU (USB serial) ┼──▶│ SQM device library → scheduler → DB layer      │──▶ MariaDB
                       │   │                          │                     │    (readings,
                       │   │   embedded HTTP server (civetweb)              │     sensors,
                       │   │     • REST API (JSON)    • static web UI        │     config_log)
                       │   └────────────────┬───────────────────────────────┘
   sqmctl CLI ─────────┘                    │
                          Browser (web UI) ◀─┴─▶ API clients
```

## Components and technology

- **SQM device library** (`src/sqm/`) — `ITransport` abstraction with `TcpTransport`
  (SQM-LE, built first) and `SerialTransport` (SQM-LU). A transport-agnostic `Protocol`
  codec parses the fixed-column responses documented in [`sqm-protocol.md`](sqm-protocol.md).
- **Database layer** (`src/db/`) — MariaDB Connector/C (`libmariadb`, LGPL-2.1) wrapped in
  an RAII C++ class. Chosen over Oracle `libmysqlclient` (GPLv2) for clean GPL-3.0 linking.
- **Daemon** (`src/daemon/`) — config parsing, a scheduler that polls each sensor every
  `interval` seconds (default 300), logging, signals, systemd integration.
- **API** (`src/api/`) — civetweb (MIT) embedded server; JSON via nlohmann/json (MIT).
- **Web UI** (`web/`) — static HTML/CSS/vanilla-JS; time-series graph via uPlot (MIT),
  vendored (no external CDNs).

## Language and build

C++17 for the core with a C-friendly device layer; CMake build. Linux x86 and ARM
(Raspberry Pi is a first-class target). License: GPL-3.0-or-later.

## Roadmap

- **M0 — Bootstrap** *(done)*: repo, CMake scaffold, skeleton daemon, CI, docs, schema draft.
- **M1 — SQM library + `sqmctl`** *(SQM-LE done)*: TCP transport, protocol codec, simulator,
  tests; USB serial transport still pending.
- **M2 — Database layer** *(done)*: `libmariadb` wrapper (`nw_db`), schema, and the `nwdb`
  CLI (register sensors, poll an SQM into the DB, query readings).
- **M3 — Daemon** *(done)*: `Scheduler` polls each active DB sensor on its
  `poll_interval_s` → DB; stderr logging + `events` table; `sigwait` shutdown/`SIGHUP`
  reload; runs under systemd. The database is the source of truth for the sensor list.
- **M4 — API**: civetweb + REST endpoints + auth.
- **M5 — Web UI**: dashboard, config, query, time-series graph.
- **M6 — Packaging & DSN export**: `.deb` for amd64/arm64/armhf; optional DSN-format export.
- **Weather integration** *(future)*: poll a co-located weather station (e.g. Ambient Weather
  WS-2000) into `weather_readings`; the schema is already in place.

The full approved plan lives outside the repo in the planning workspace; this file is the
in-repo summary kept current as milestones land.
