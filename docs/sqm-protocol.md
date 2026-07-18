<!--
  Author:        David Gilinsky
  File:          docs/sqm-protocol.md
  Purpose:       Reference for the Unihedron SQM serial/TCP command+response protocol.
  Created:       2026-07-18
  Last Modified: 2026-07-18
  Version:       0.1.0
-->

# Unihedron SQM command/response reference

Extracted from the Unihedron *SQM-LU Operator's Manual* (v20260615), section 8.
The **same command set** applies to the SQM-LE over TCP (default **port 10001**);
the SQM-LU uses USB serial (**115200 baud, 8N1**, FTDI, typically `/dev/ttyUSB*`).

Response fields are **fixed-column**. The manual guarantees columns 0–54 of the reading
response are stable across firmware versions; parse by position, not by splitting.

## Standard commands (Table 8.1)

| Command | Description |
|---------|-------------|
| `rx` `Rx` | Reading request (averaged — last 8 period-mode readings) |
| `ux` `Ux` | Unaveraged (most recent) reading request |
| `rfx` `rFx` | Linear / inline-linear reading requests |
| `cx` | Calibration information request |
| `ix` | Unit information request (lower-case `i`) |
| `zcalAx` | Arm light calibration |
| `zcalBx` | Arm dark calibration |
| `zcalDx` | Disarm calibration; also reports lock-switch status |
| `zcal5` / `zcal6` / `zcal7` / `zcal8` | Manually set light offset / light temp / dark period / dark temp |
| `P…x` / `p…x` | Set interval-report period (EEPROM+RAM / RAM only). Feature ≥ 13 |
| `T…x` / `t…x` | Set interval-report threshold (EEPROM+RAM / RAM only). Feature ≥ 13 |
| `Ix` | Request interval settings (upper-case `I`). Feature ≥ 13 |
| `Yx` `YRx` `Yrx` … | Continuous-reporting status / enable / disable |
| `sx` | Request reading of internal variables |
| `S…x` | Simulate internal calculations |
| `0x19` | Reset micro-controller |

## Reading request `rx` (Table 8.2)

Example response:

```
r, 06.70m,0000022921Hz,0000000020c,0000000.000s, 039.4C<CR><LF>
0         1         2         3         4         5
0123456789012345678901234567890123456789012345678901234 5 6
```

| Columns | Example | Field |
|---------|---------|-------|
| 0 | `r` | Record type (reading) |
| 2–8 | `␣06.70m` | Sky brightness, **mag/arcsec²** (leading space = positive, `-` = negative). `0.00m` = at upper brightness limit |
| 10–21 | `0000022921Hz` | Sensor frequency (Hz) |
| 23–33 | `0000000020c` | Sensor period in counts (counts occur at 460 800 Hz = 14.7456 MHz / 32) |
| 35–46 | `0000000.000s` | Sensor period in seconds (counts ÷ 460 800) |
| 48–54 | `␣039.4C` | Sensor temperature (°C), leading space/`-` sign |
| 55–56 | `<CR><LF>` | Terminator |

- `ux` returns the same layout but the most-recent (un-averaged) value.
- `r1x` appends a freshness status and the un-averaged reading (Table 8.3).

## Unit information `ix` (Table 8.7)

```
i,00000002,00000003,00000001,00000413<CR><LF>
```

| Columns | Example | Field |
|---------|---------|-------|
| 0 | `i` | Record type (unit info) |
| 2–9 | `00000002` | Protocol number (8 digits) |
| 11–18 | `00000003` | Model number (8 digits) |
| 20–27 | `00000001` | Feature number (8 digits) |
| 29–36 | `00000413` | Serial number (8 digits) |

## Calibration information `cx` (Table 8.8)

```
c,00000017.60m,0000000.000s, 039.4C,00000008.71m, 039.4C<CR><LF>
```

| Columns | Example | Field |
|---------|---------|-------|
| 0 | `c` | Record type (calibration) |
| 2–13 | `00000017.60m` | Light calibration offset (mag/arcsec²) |
| 15–26 | `0000000.000s` | Dark calibration time period (s) |
| 28–34 | `␣039.4C` | Temperature during light calibration (°C) |
| 36–47 | `00000008.71m` | Factory reference sensor offset (mag/arcsec²) |
| 49–55 | `␣039.4C` | Temperature during dark calibration (°C) |

## Interval reporting (feature ≥ 13)

The unit can push a report every interval. The report uses the **same layout as the
reading response**, with the 8-digit serial number appended at the end (feature ≥ 14) so
multiple reporting units can be distinguished. Configure with `P…x`/`p…x` (period) and
`T…x`/`t…x` (threshold); query current settings with `Ix`.

## Implementation notes (for `src/sqm/`)

- Read until `<CR><LF>`; validate the leading record-type character (`r`/`u`/`c`/`i`/`Y`).
- Parse mag/temperature by fixed column ranges; the sign column is a space or `-`.
- Wrap transport (TCP vs serial) behind one `ITransport` interface; the protocol codec is
  transport-agnostic.
- A software **SQM simulator** that emits these exact strings enables unit tests without hardware.

*Primary source: <https://www.unihedron.com/projects/darksky/cd/SQM-LU/SQM-LU_Users_manual.pdf>.
See also the SQM-LE manual for Ethernet specifics (TCP port 10001).*
