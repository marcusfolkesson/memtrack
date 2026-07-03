# cmake/arm-linux-toolchain.cmake
# ARM 32-bit Linux cross-compilation toolchain file.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-linux-toolchain.cmake
#
# Set the CROSS_TOOLCHAIN environment variable (or CMake cache var) to the
# toolchain prefix, e.g.:
#   CROSS_TOOLCHAIN=arm-linux-gnueabihf cmake -B build ...
# Defaults to "arm-linux-gnueabihf" if unset.

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(DEFINED ENV{CROSS_TOOLCHAIN})
    set(_toolchain_prefix "$ENV{CROSS_TOOLCHAIN}")
elseif(DEFINED CROSS_TOOLCHAIN)
    set(_toolchain_prefix "${CROSS_TOOLCHAIN}")
else()
    set(_toolchain_prefix "arm-linux-gnueabihf")
endif()

set(CMAKE_C_COMPILER   ${_toolchain_prefix}-gcc)
set(CMAKE_CXX_COMPILER ${_toolchain_prefix}-g++)
set(CMAKE_ASM_COMPILER ${_toolchain_prefix}-gcc)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # find host programs (cmake scripts etc.)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)    # find target libraries
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)    # find target headers
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
