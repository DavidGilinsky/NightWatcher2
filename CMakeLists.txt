# ---------------------------------------------------------------------------
# Author:        David Gilinsky
# File:          CMakeLists.txt
# Purpose:       Top-level CMake build: targets, options, install rules, tests.
# Created:       2026-07-18
# Last Modified: 2026-07-18
# Version:       0.1.0
# License:       GPL-3.0-or-later
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

project(NightWatcher2
    VERSION 0.1.0
    DESCRIPTION "Tools to interface Unihedron Sky Quality Meters with the Dark Sky Network"
    HOMEPAGE_URL "https://github.com/DavidGilinsky/NightWatcher2"
    LANGUAGES C CXX)

# ---- Language standards ----
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a reasonable build type for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Install as a self-contained bundle: everything lives under one prefix so the
# install is clean and trivial to remove. Default /usr/local/nightwatcher;
# an explicit -DCMAKE_INSTALL_PREFIX still wins.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr/local/nightwatcher" CACHE PATH
        "Install prefix (self-contained bundle)" FORCE)
endif()

option(NW_BUILD_TESTS "Build unit tests" ON)
option(NW_WITH_DB "Build the MariaDB database layer (requires libmariadb-dev)" ON)

find_package(Threads REQUIRED)

# ---- Shared warning flags ----
add_library(nw_warnings INTERFACE)
target_compile_options(nw_warnings INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall;-Wextra;-Wpedantic>)

# ---- Generated version header ----
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nightwatcher/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/nightwatcher/version.hpp
    @ONLY)

# ---- Core library (shared by the daemon and the tests) ----
add_library(nw_core STATIC
    src/daemon/config.cpp
    src/daemon/logging.cpp)
target_include_directories(nw_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(nw_core PUBLIC nw_warnings)

# ---- SQM device library ----
add_library(nw_sqm STATIC
    src/sqm/protocol.cpp
    src/sqm/tcp_transport.cpp
    src/sqm/serial_transport.cpp
    src/sqm/sqm_device.cpp
    src/sqm/device_factory.cpp
    src/sqm/discovery.cpp)
target_include_directories(nw_sqm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sqm
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(nw_sqm PUBLIC nw_warnings Threads::Threads)

# ---- sqmctl CLI ----
add_executable(sqmctl src/cli/sqmctl.cpp)
target_link_libraries(sqmctl PRIVATE nw_sqm)

# ---- Database layer (optional; requires libmariadb-dev) ----
if(NW_WITH_DB)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MARIADB REQUIRED libmariadb)
    find_package(OpenSSL REQUIRED)

    # ---- Vendored httplib with OpenSSL client support (one config across all TUs) ----
    add_library(nw_httplib INTERFACE)
    target_include_directories(nw_httplib SYSTEM INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib)
    target_compile_definitions(nw_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(nw_httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

    # ---- Auth / crypto (PBKDF2 password hashing + random tokens via OpenSSL) ----
    add_library(nw_auth STATIC src/auth/password.cpp)
    target_include_directories(nw_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
    target_link_libraries(nw_auth PUBLIC nw_warnings OpenSSL::Crypto)

    add_library(nw_db STATIC src/db/database.cpp)
    target_include_directories(nw_db PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db
        ${CMAKE_CURRENT_SOURCE_DIR}/src/sqm
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${MARIADB_INCLUDE_DIRS})
    target_link_directories(nw_db PUBLIC ${MARIADB_LIBRARY_DIRS})
    target_link_libraries(nw_db PUBLIC nw_warnings ${MARIADB_LIBRARIES})

    # ---- Weather providers + scheduler (modular, pull-based) ----
    add_library(nw_weather STATIC
        src/weather/provider.cpp
        src/weather/weather_scheduler.cpp)
    target_include_directories(nw_weather PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/weather)
    target_link_libraries(nw_weather PUBLIC nw_db nw_core nw_httplib)

    # ---- Data-export module (DSN / Globe at Night; modular, like weather) ----
    add_library(nw_export STATIC
        src/export/exporter.cpp
        src/export/dsn_format.cpp
        src/export/gdrive.cpp
        src/export/timeutil.cpp
        src/export/export_runner.cpp
        src/export/export_scheduler.cpp)
    target_include_directories(nw_export PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/export)
    target_link_libraries(nw_export PUBLIC nw_db nw_core nw_httplib)

    # ---- One-time headless OAuth helper for Google Drive ----
    add_executable(nwexport-auth src/cli/gdrive_auth.cpp)
    target_link_libraries(nwexport-auth PRIVATE nw_export)

    add_executable(nwdb src/cli/nwdb.cpp)
    target_link_libraries(nwdb PRIVATE nw_db nw_sqm)

    # ---- HTTP/JSON API (embedded in the daemon; cpp-httplib + nlohmann/json) ----
    add_library(nw_api STATIC src/api/http_server.cpp src/api/tls_cert.cpp)
    target_include_directories(nw_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/api)
    target_link_libraries(nw_api PUBLIC nw_db nw_sqm nw_core nw_auth nw_weather nw_export nw_httplib OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

    # The daemon records to the database and serves the API, so it needs the DB layer.
    add_executable(nightwatcherd src/daemon/main.cpp src/daemon/scheduler.cpp)
    target_link_libraries(nightwatcherd PRIVATE nw_core nw_sqm nw_db nw_api nw_weather nw_export Threads::Threads)
endif()

# ---- Install (self-contained bundle under CMAKE_INSTALL_PREFIX) ----
# Layout, flattened directly under the prefix:
#   <prefix>/bin  <prefix>/web  <prefix>/sql  <prefix>/share  <prefix>/lib/systemd/system
# PATH and systemd discovery are handled with symlinks (see NW_INSTALL_SYSTEM_LINKS).
set(NW_SYMLINK_BINDIR "/usr/local/bin" CACHE PATH
    "Directory to symlink the CLIs into so they land on PATH")
option(NW_INSTALL_SYSTEM_LINKS
    "At install time, create the PATH/systemd symlinks and seed /etc/nightwatcher" ON)

# Bake the real prefix into the config example and the systemd unit.
configure_file(config/nightwatcher.conf.example.in
               ${CMAKE_CURRENT_BINARY_DIR}/nightwatcher.conf.example @ONLY)
configure_file(config/systemd/nightwatcherd.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/nightwatcherd.service @ONLY)

install(TARGETS sqmctl RUNTIME DESTINATION bin)
if(NW_WITH_DB)
    install(TARGETS nightwatcherd nwdb nwexport-auth RUNTIME DESTINATION bin)
endif()
install(DIRECTORY web/ DESTINATION web)
install(FILES sql/schema.sql sql/setup.sql DESTINATION sql)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nightwatcher.conf.example DESTINATION share)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nightwatcherd.service DESTINATION lib/systemd/system)
install(FILES config/udev/99-nightwatcher-sqm.rules DESTINATION lib/udev/rules.d)

# Bundled uninstaller + a `make uninstall` convenience target.
configure_file(cmake/uninstall.sh.in ${CMAKE_CURRENT_BINARY_DIR}/uninstall.sh @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/uninstall.sh DESTINATION .)
add_custom_target(uninstall COMMAND ${CMAKE_CURRENT_BINARY_DIR}/uninstall.sh)

# Reach outside the prefix: CLI symlinks on PATH, systemd unit link, /etc config.
# The .deb does this in its postinst instead (built with NW_INSTALL_SYSTEM_LINKS=OFF).
if(NW_INSTALL_SYSTEM_LINKS)
    configure_file(cmake/nw_install_links.cmake.in
                   ${CMAKE_CURRENT_BINARY_DIR}/nw_install_links.cmake @ONLY)
    install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/nw_install_links.cmake)
endif()

# ---- Packaging (CPack: Debian .deb) ----
# Build one with:
#   cmake -B pkg -DNW_INSTALL_SYSTEM_LINKS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build pkg --parallel && ( cd pkg && cpack -G DEB )
# The payload is just the bundle under the prefix; the package's postinst creates
# the PATH/systemd/udev symlinks, seeds /etc config, and enables the service.
set(CPACK_PACKAGE_NAME "nightwatcher")
set(CPACK_PACKAGE_VENDOR "SAZ Dark Sky Association")
set(CPACK_PACKAGE_CONTACT "David Gilinsky <gilinsky@gilinskyresearch.com>")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "science")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)             # auto Depends from the ELF binaries
set(CPACK_DEBIAN_PACKAGE_DEPENDS "adduser, debconf (>= 0.5) | debconf-2.0")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mariadb-server | mysql-server")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)  # 0755 maintainer scripts
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")          # nightwatcher_<ver>_<arch>.deb

# Interactive install-time configuration is debconf-driven: `config` asks the
# questions in `templates`, and `postinst` applies the answers (DB connection +
# password, API bind/port/TLS, optional DB creation). postinst/prerm/postrm are
# generated from their .in templates for @CMAKE_INSTALL_PREFIX@ substitution;
# config/templates are static and shipped from the source tree.
configure_file(debian/postinst.in ${CMAKE_CURRENT_BINARY_DIR}/debian/postinst @ONLY)
configure_file(debian/prerm.in    ${CMAKE_CURRENT_BINARY_DIR}/debian/prerm    @ONLY)
configure_file(debian/postrm.in   ${CMAKE_CURRENT_BINARY_DIR}/debian/postrm   @ONLY)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_BINARY_DIR}/debian/postinst"
    "${CMAKE_CURRENT_BINARY_DIR}/debian/prerm"
    "${CMAKE_CURRENT_BINARY_DIR}/debian/postrm"
    "${CMAKE_CURRENT_SOURCE_DIR}/debian/config"
    "${CMAKE_CURRENT_SOURCE_DIR}/debian/templates")

include(CPack)

# ---- Tests ----
if(NW_BUILD_TESTS)
    enable_testing()
    add_executable(config_test tests/config_test.cpp)
    target_link_libraries(config_test PRIVATE nw_core)
    add_test(NAME config_test COMMAND config_test)

    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE nw_sqm)
    add_test(NAME protocol_test COMMAND protocol_test)

    add_executable(discovery_test tests/discovery_test.cpp)
    target_link_libraries(discovery_test PRIVATE nw_sqm)
    add_test(NAME discovery_test COMMAND discovery_test)

    add_executable(serial_transport_test tests/serial_transport_test.cpp)
    target_link_libraries(serial_transport_test PRIVATE nw_sqm)
    add_test(NAME serial_transport_test COMMAND serial_transport_test)

    if(NW_WITH_DB)
        add_executable(db_test tests/db_test.cpp)
        target_link_libraries(db_test PRIVATE nw_db nw_sqm)
        add_test(NAME db_test COMMAND db_test)

        add_executable(api_test tests/api_test.cpp)
        target_link_libraries(api_test PRIVATE nw_api)
        add_test(NAME api_test COMMAND api_test)

        add_executable(tls_cert_test tests/tls_cert_test.cpp)
        target_link_libraries(tls_cert_test PRIVATE nw_api OpenSSL::Crypto)
        add_test(NAME tls_cert_test COMMAND tls_cert_test)

        add_executable(weather_test tests/weather_test.cpp)
        target_link_libraries(weather_test PRIVATE nw_weather)
        add_test(NAME weather_test COMMAND weather_test)

        add_executable(export_test tests/export_test.cpp)
        target_link_libraries(export_test PRIVATE nw_export)
        add_test(NAME export_test COMMAND export_test)

        add_executable(gdrive_test tests/gdrive_test.cpp)
        target_link_libraries(gdrive_test PRIVATE nw_export OpenSSL::Crypto)
        add_test(NAME gdrive_test COMMAND gdrive_test)

        add_executable(timeutil_test tests/timeutil_test.cpp)
        target_link_libraries(timeutil_test PRIVATE nw_export)
        add_test(NAME timeutil_test COMMAND timeutil_test)
    endif()

    # Software SQM simulator (TCP) for local end-to-end testing without hardware.
    add_executable(sqm-sim tools/sqm-sim.cpp)
    target_link_libraries(sqm-sim PRIVATE nw_warnings)
endif()

message(STATUS "NightWatcher2 ${PROJECT_VERSION} — build type: ${CMAKE_BUILD_TYPE}, tests: ${NW_BUILD_TESTS}")
