-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/schema.sql
-- Purpose:       MariaDB/MySQL schema: sensors, readings, config_log, events,
--                weather_stations/weather_readings, users/sessions, and
--                export_targets/export_log tables.
-- Created:       2026-07-18
-- Last Modified: 2026-07-18
-- Version:       0.1.0
-- License:       GPL-3.0-or-later
-- ---------------------------------------------------------------------------
-- NightWatcher2 database schema (MariaDB / MySQL).
--
--   (load order: setup.sql first, then this file)
--
-- Conventions:
--   * Timestamps are stored in UTC; application code supplies UTC values.
--   * Stored units are metric/SI (deg C, m/s, mm, hPa); ingest code converts.
--   * raw_line / raw columns keep the exact device payload so any field we did
--     not break out can be backfilled later without recreating tables.

-- The database and application user are created by setup.sql (run that first).
USE nightwatcher;

-- One row per Sky Quality Meter / station.
CREATE TABLE IF NOT EXISTS sensors (
    id              VARCHAR(32)  NOT NULL,               -- DSN-style id, e.g. 'DSN003'
    name            VARCHAR(128) NULL,                   -- e.g. 'Sugarloaf'
    site            VARCHAR(64)  NULL,                   -- groups co-located instruments
    serial_number   VARCHAR(16)  NULL,                   -- Unihedron 8-digit serial
    model           VARCHAR(32)  NULL,                   -- e.g. 'SQM-LE', 'SQM-LU'
    protocol_ver    INT          NULL,                   -- from 'ix' unit-info response
    feature_ver     INT          NULL,                   -- from 'ix' unit-info response
    latitude        DECIMAL(9,6) NULL,
    longitude       DECIMAL(9,6) NULL,
    elevation_m     DECIMAL(7,1) NULL,
    timezone        VARCHAR(64)  NULL,                   -- IANA tz, e.g. 'America/Phoenix'
    transport       ENUM('tcp','serial') NOT NULL,
    address         VARCHAR(128) NOT NULL,               -- 'host:port' or '/dev/ttyUSB0'
    poll_interval_s INT          NOT NULL DEFAULT 300,   -- per-sensor cadence (seconds)
    status          ENUM('active','inactive','retired') NOT NULL DEFAULT 'active',
    installed_at    DATE         NULL,
    notes           TEXT         NULL,
    created_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id)
) ENGINE=InnoDB;

-- One row per SQM reading (rx / ux / interval report).
CREATE TABLE IF NOT EXISTS readings (
    id             BIGINT       NOT NULL AUTO_INCREMENT,
    sensor_id      VARCHAR(32)  NOT NULL,
    ts_utc         DATETIME     NOT NULL,                -- time the reading was taken (UTC)
    mag_arcsec2    DECIMAL(6,3) NOT NULL,                -- sky brightness, mag/arcsec^2
    freq_hz        BIGINT       NULL,                    -- sensor frequency (Hz)
    period_counts  BIGINT       NULL,                    -- sensor period (counts @ 460800 Hz)
    period_s       DECIMAL(12,3) NULL,                   -- sensor period (seconds)
    temp_c         DECIMAL(5,1) NULL,                    -- sensor temperature (deg C)
    quality        ENUM('ok','saturated','suspect') NOT NULL DEFAULT 'ok',
    source         ENUM('poll','interval') NOT NULL DEFAULT 'poll',
    raw_line       VARCHAR(128) NULL,                    -- exact device response, for audit
    created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_sensor_ts (sensor_id, ts_utc),
    KEY idx_sensor_ts (sensor_id, ts_utc),
    CONSTRAINT fk_readings_sensor FOREIGN KEY (sensor_id)
        REFERENCES sensors (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Configuration / calibration snapshots and changes (from 'cx', interval settings, etc.).
CREATE TABLE IF NOT EXISTS config_log (
    id                BIGINT       NOT NULL AUTO_INCREMENT,
    sensor_id         VARCHAR(32)  NOT NULL,
    ts_utc            DATETIME     NOT NULL,
    event_type        ENUM('calibration','config_change') NOT NULL,
    light_cal_offset  DECIMAL(8,2) NULL,                 -- cx: light calibration offset (mag)
    dark_cal_period_s DECIMAL(12,3) NULL,                -- cx: dark calibration period (s)
    temp_light_c      DECIMAL(5,1) NULL,                 -- cx: temp during light calibration
    sensor_offset     DECIMAL(8,2) NULL,                 -- cx: factory reference sensor offset (mag)
    temp_dark_c       DECIMAL(5,1) NULL,                 -- cx: temp during dark calibration
    interval_s        INT          NULL,                 -- interval-reporting period
    threshold         DECIMAL(6,3) NULL,                 -- interval-reporting threshold
    raw               VARCHAR(255) NULL,                 -- exact device response, for audit
    changed_by        VARCHAR(64)  NULL,                 -- user / process that made the change
    note              TEXT         NULL,
    created_at        DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_cfg_sensor_ts (sensor_id, ts_utc),
    CONSTRAINT fk_config_sensor FOREIGN KEY (sensor_id)
        REFERENCES sensors (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Operational event log: device up/down, connection errors, daemon lifecycle.
-- device_id is a free reference (no FK) so events survive device deletion and
-- may point at a sensor OR a weather station, or be NULL (daemon-wide).
CREATE TABLE IF NOT EXISTS events (
    id          BIGINT       NOT NULL AUTO_INCREMENT,
    ts_utc      DATETIME     NOT NULL,
    device_id   VARCHAR(32)  NULL,                       -- sensor or weather-station id, or NULL
    source      VARCHAR(32)  NOT NULL,                   -- 'daemon','sqm','weather','api'
    level       ENUM('info','warning','error') NOT NULL DEFAULT 'info',
    event       VARCHAR(64)  NOT NULL,                   -- 'started','connect','disconnect','read_error'
    detail      TEXT         NULL,
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_events_ts (ts_utc),
    KEY idx_events_device_ts (device_id, ts_utc)
) ENGINE=InnoDB;

-- Separate weather stations (e.g. Ambient Weather WS-2000) co-located with SQMs.
-- Polling is a future module; the tables exist now so no migration is needed later.
CREATE TABLE IF NOT EXISTS weather_stations (
    id              VARCHAR(32)  NOT NULL,               -- e.g. 'WX001'
    name            VARCHAR(128) NULL,
    site            VARCHAR(64)  NULL,                   -- shared with a co-located sensor
    model           VARCHAR(64)  NULL,                   -- e.g. 'Ambient Weather WS-2000'
    provider        VARCHAR(32)  NULL,                   -- 'ambientweather' | 'wunderground'
    config          TEXT         NULL,                   -- JSON: provider settings (may include secrets)
    transport       VARCHAR(16)  NULL,                   -- legacy; providers use `config`
    address         VARCHAR(255) NULL,                   -- legacy; providers use `config`
    latitude        DECIMAL(9,6) NULL,
    longitude       DECIMAL(9,6) NULL,
    elevation_m     DECIMAL(7,1) NULL,
    timezone        VARCHAR(64)  NULL,
    poll_interval_s INT          NOT NULL DEFAULT 300,
    status          ENUM('active','inactive','retired') NOT NULL DEFAULT 'active',
    installed_at    DATE         NULL,
    notes           TEXT         NULL,
    created_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id)
) ENGINE=InnoDB;

-- One row per weather observation. Stored units are metric/SI; ingest converts.
CREATE TABLE IF NOT EXISTS weather_readings (
    id                BIGINT       NOT NULL AUTO_INCREMENT,
    station_id        VARCHAR(32)  NOT NULL,
    ts_utc            DATETIME     NOT NULL,
    temp_c            DECIMAL(5,1) NULL,                 -- outdoor temperature
    humidity_pct      DECIMAL(5,1) NULL,                 -- outdoor relative humidity
    dew_point_c       DECIMAL(5,1) NULL,
    pressure_hpa      DECIMAL(7,2) NULL,                 -- relative (sea-level) pressure
    pressure_abs_hpa  DECIMAL(7,2) NULL,                 -- absolute (station) pressure
    wind_speed_ms     DECIMAL(6,2) NULL,
    wind_gust_ms      DECIMAL(6,2) NULL,
    wind_dir_deg      SMALLINT     NULL,                 -- 0-359
    rain_rate_mmh     DECIMAL(6,2) NULL,                 -- current rain rate
    rain_daily_mm     DECIMAL(7,2) NULL,                 -- daily accumulation
    uv_index          DECIMAL(4,1) NULL,
    solar_wm2         DECIMAL(7,1) NULL,                 -- solar irradiance (W/m^2)
    cloud_cover_pct   DECIMAL(5,1) NULL,                 -- if available from a sky/cloud sensor
    source            VARCHAR(32)  NOT NULL DEFAULT 'poll',
    raw               TEXT         NULL,                 -- full station payload (JSON), for audit
    created_at        DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_wx_station_ts (station_id, ts_utc),
    KEY idx_wx_station_ts (station_id, ts_utc),
    CONSTRAINT fk_weather_station FOREIGN KEY (station_id)
        REFERENCES weather_stations (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Web-UI / API users. Passwords are stored as PBKDF2-HMAC-SHA256 (salted); the
-- daemon seeds an 'admin' account on first start if this table is empty.
CREATE TABLE IF NOT EXISTS users (
    id                   BIGINT      NOT NULL AUTO_INCREMENT,
    username             VARCHAR(64) NOT NULL,
    password_hash        VARCHAR(255) NOT NULL,   -- pbkdf2_sha256$iters$salt$hash
    role                 ENUM('admin','viewer') NOT NULL DEFAULT 'admin',
    must_change_password TINYINT(1)  NOT NULL DEFAULT 0,
    created_at           DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_users_username (username)
) ENGINE=InnoDB;

-- Login sessions. token is a random hex string stored in a cookie.
CREATE TABLE IF NOT EXISTS sessions (
    token      CHAR(64) NOT NULL,
    user_id    BIGINT   NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at DATETIME NOT NULL,
    PRIMARY KEY (token),
    KEY idx_sessions_user (user_id),
    CONSTRAINT fk_sessions_user FOREIGN KEY (user_id)
        REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Data-export targets: push a sensor's readings to an external network (Dark Sky
-- Network Google Drive, Globe at Night, ...). Modular like the weather providers:
-- `target` selects the exporter, `config` (JSON) holds its per-endpoint settings.
CREATE TABLE IF NOT EXISTS export_targets (
    id              VARCHAR(32)  NOT NULL,               -- e.g. 'dsn-036'
    sensor_id       VARCHAR(32)  NOT NULL,               -- SQM whose readings are exported
    name            VARCHAR(128) NULL,
    target          VARCHAR(32)  NOT NULL,               -- 'dsn' | 'globeatnight'
    config          TEXT         NULL,                   -- JSON endpoint settings (may include secrets)
    schedule        ENUM('nightly','manual','interval') NOT NULL DEFAULT 'nightly',
    schedule_time   VARCHAR(5)   NULL,                   -- 'HH:MM' local, for the nightly schedule
    interval_s      INT          NULL,                   -- period for the 'interval' schedule
    last_export_ts  DATETIME     NULL,                   -- watermark: newest reading ts_utc exported
    status          ENUM('active','inactive','retired') NOT NULL DEFAULT 'active',
    notes           TEXT         NULL,
    created_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_export_sensor (sensor_id),
    CONSTRAINT fk_export_sensor FOREIGN KEY (sensor_id)
        REFERENCES sensors (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- One row per export run (audit trail). target_id is a free reference (no FK) so
-- the log survives target deletion, like the events table.
CREATE TABLE IF NOT EXISTS export_log (
    id          BIGINT       NOT NULL AUTO_INCREMENT,
    target_id   VARCHAR(32)  NOT NULL,
    ts_utc      DATETIME     NOT NULL,                   -- when the export ran (UTC)
    from_ts     DATETIME     NULL,                       -- reading window covered (UTC)
    to_ts       DATETIME     NULL,
    row_count   INT          NOT NULL DEFAULT 0,         -- readings exported
    file_name   VARCHAR(255) NULL,                       -- generated file name
    remote_id   VARCHAR(255) NULL,                       -- Drive file id / URL
    status      ENUM('ok','error','empty') NOT NULL DEFAULT 'ok',
    detail      TEXT         NULL,                        -- error message / notes
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_export_log_target_ts (target_id, ts_utc)
) ENGINE=InnoDB;

-- Migrations for existing databases (idempotent; MariaDB IF NOT EXISTS).
ALTER TABLE weather_stations ADD COLUMN IF NOT EXISTS provider VARCHAR(32) NULL;
ALTER TABLE weather_stations ADD COLUMN IF NOT EXISTS config   TEXT        NULL;
