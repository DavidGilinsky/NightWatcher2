# ---------------------------------------------------------------------------
# Author:        David Gilinsky
# File:          CMakeLists.txt
# Purpose:       Top-level CMake build: targets, options, install rules, tests.
# Created:       2026-07-18
# Last Modified: 2026-07-18
# Version:       0.1.0
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

option(NW_BUILD_TESTS "Build unit tests" ON)

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
    src/daemon/config.cpp)
target_include_directories(nw_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(nw_core PUBLIC nw_warnings)

# ---- Daemon executable ----
add_executable(nightwatcherd src/daemon/main.cpp)
target_link_libraries(nightwatcherd PRIVATE nw_core)

# ---- Install rules ----
include(GNUInstallDirs)
install(TARGETS nightwatcherd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/nightwatcher.conf.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/nightwatcher)
install(FILES config/systemd/nightwatcherd.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)

# ---- Tests ----
if(NW_BUILD_TESTS)
    enable_testing()
    add_executable(config_test tests/config_test.cpp)
    target_link_libraries(config_test PRIVATE nw_core)
    add_test(NAME config_test COMMAND config_test)
endif()

message(STATUS "NightWatcher2 ${PROJECT_VERSION} — build type: ${CMAKE_BUILD_TYPE}, tests: ${NW_BUILD_TESTS}")
