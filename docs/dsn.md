<!--
  Author:        David Gilinsky
  File:          docs/dsn.md
  Purpose:       Notes on the Dark Sky Network data and submission format.
  Created:       2026-07-18
  Last Modified: 2026-07-18
  Version:       0.1.0
-->

# Dark Sky Network (DSN) notes

Reference: <https://soazcomms.github.io/docs/DSN_Description.html>

The DSN, operated by the Southern Arizona Dark Sky Association, aggregates sky-brightness
data from sensors across Southern Arizona.

## What we know

- Sensors record **every 5 minutes**.
- Sky brightness is reported in **magnitudes per square arc-second (mag/arcsec²)**; the
  network also uses **radiance in nW/cm²/sr** (derived from the magnitude).
- Two sensor families: **SQM** units (manual data download every few months) and **TESS**
  sensors (transmit over WiFi). NightWatcher2 targets the SQM family and removes the
  manual-download step by logging continuously to a database.
- Stations carry an id such as `DSN003` with a site label, e.g. `DSN003-S_CrestaLoma`.

## Open item

The exact ingest/submission format the DSN expects (columns, units, transport) is **not
published** on the description page. Our database schema captures everything an SQM
produces (timestamp, mag/arcsec², frequency, period, temperature, serial, calibration), so
a DSN export can be derived later. **Before building the M6 export we must obtain the
concrete DSN ingest spec from soazcomms.**

## Derived quantity

Radiance (nW/cm²/sr) is a function of the magnitude reading; the conversion constant will
be confirmed against the DSN's published methodology and implemented in the DB/API layer
when M6 is scoped.
