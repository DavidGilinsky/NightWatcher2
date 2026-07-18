-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/setup.sql
-- Purpose:       Create the NightWatcher2 database, application user, and grants.
--                Run once as an administrator, THEN load the schema:
--                    sudo mariadb < sql/setup.sql
--                    sudo mariadb < sql/schema.sql
-- Created:       2026-07-18
-- Last Modified: 2026-07-18
-- Version:       0.1.0
-- License:       GPL-3.0-or-later
-- ---------------------------------------------------------------------------
-- NOTE: 'nightwatcher' is a development password. Change it for any real
-- deployment and set NW_DB_PASSWORD in the environment to match.

CREATE DATABASE IF NOT EXISTS nightwatcher
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS 'nightwatcher'@'localhost' IDENTIFIED BY 'nightwatcher';
CREATE USER IF NOT EXISTS 'nightwatcher'@'127.0.0.1' IDENTIFIED BY 'nightwatcher';

GRANT ALL PRIVILEGES ON nightwatcher.* TO 'nightwatcher'@'localhost';
GRANT ALL PRIVILEGES ON nightwatcher.* TO 'nightwatcher'@'127.0.0.1';

FLUSH PRIVILEGES;
