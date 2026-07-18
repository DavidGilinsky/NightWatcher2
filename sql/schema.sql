-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/schema.sql
-- Purpose:       MariaDB/MySQL schema: sensors, readings, and config_log tables.
-- Created:       2026-07-18
-- Last Modified: 2026-07-18
-- Version:       0.1.0
-- ---------------------------------------------------------------------------
-- NightWatcher2 database schema (MariaDB / MySQL).
-- Draft for Milestone 0; refined and migrated in Milestone 2.
--
--   mysql -u root -p < sql/schema.sql
--
-- Time is stored in UTC. Application code is responsible for supplying UTC values.

CREATE DATABASE IF NOT EXISTS nightwatcher
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE nightwatcher;

-- One row per Sky Quality Meter / station.
CREATE TABLE IF NOT EXISTS sensors (
    id             VARCHAR(32)  NOT NULL,              -- DSN-style id, e.g. 'DSN003'
    name           VARCHAR(128) NULL,                  -- e.g. 'Sugarloaf'
    serial_number  VARCHAR(16)  NULL,                  -- Unihedron 8-digit serial
    model          VARCHAR(32)  NULL,                  -- e.g. 'SQM-LE', 'SQM-LU'
    protocol_ver   INT          NULL,                  -- from 'ix' unit-info response
    feature_ver    INT          NULL,                  -- from 'ix' unit-info response
    latitude       DECIMAL(9,6) NULL,
    longitude      DECIMAL(9,6) NULL,
    elevation_m    DECIMAL(7,1) NULL,
    transport      ENUM('tcp','serial') NOT NULL,
    address        VARCHAR(128) NOT NULL,              -- 'host:port' or '/dev/ttyUSB0'
    notes          TEXT         NULL,
    created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id)
) ENGINE=InnoDB;

-- One row per SQM reading (rx / ux / interval report).
CREATE TABLE IF NOT EXISTS readings (
    id             BIGINT       NOT NULL AUTO_INCREMENT,
    sensor_id      VARCHAR(32)  NOT NULL,
    ts_utc         DATETIME     NOT NULL,              -- time the reading was taken (UTC)
    mag_arcsec2    DECIMAL(6,3) NOT NULL,              -- sky brightness, mag/arcsec^2
    freq_hz        BIGINT       NULL,                  -- sensor frequency (Hz)
    period_counts  BIGINT       NULL,                  -- sensor period (counts @ 460800 Hz)
    period_s       DECIMAL(12,3) NULL,                 -- sensor period (seconds)
    temp_c         DECIMAL(5,1) NULL,                  -- sensor temperature (deg C)
    source         ENUM('poll','interval') NOT NULL DEFAULT 'poll',
    raw_line       VARCHAR(128) NULL,                  -- exact device response, for audit
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
    light_cal_offset  DECIMAL(8,2) NULL,               -- cx: light calibration offset (mag)
    dark_cal_period_s DECIMAL(12,3) NULL,              -- cx: dark calibration period (s)
    temp_light_c      DECIMAL(5,1) NULL,               -- cx: temp during light calibration
    temp_dark_c       DECIMAL(5,1) NULL,               -- cx: temp during dark calibration
    interval_s        INT          NULL,               -- interval-reporting period
    threshold         DECIMAL(6,3) NULL,               -- interval-reporting threshold
    raw               VARCHAR(255) NULL,               -- exact device response, for audit
    changed_by        VARCHAR(64)  NULL,               -- user / process that made the change
    note              TEXT         NULL,
    created_at        DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_cfg_sensor_ts (sensor_id, ts_utc),
    CONSTRAINT fk_config_sensor FOREIGN KEY (sensor_id)
        REFERENCES sensors (id) ON DELETE CASCADE
) ENGINE=InnoDB;
