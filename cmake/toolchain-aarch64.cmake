# ---------------------------------------------------------------------------
# Author:        David Gilinsky
# File:          cmake/toolchain-aarch64.cmake
# Purpose:       CMake toolchain file for cross-compiling to 64-bit ARM (aarch64).
# Created:       2026-07-18
# Last Modified: 2026-07-18
# Version:       0.1.0
# ---------------------------------------------------------------------------
# Cross-compile toolchain for 64-bit ARM (aarch64), e.g. Raspberry Pi OS 64-bit.
#
# Prerequisite (Debian/Ubuntu host):  sudo apt install g++-aarch64-linux-gnu
# Usage:
#   cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
#   cmake --build build-arm64

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Search host for programs, target sysroot for libs/headers.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
