# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the bridge library, its tests against the simulated back cover, and the bench tools.
# The firmware does not use this file: it takes the library through library.json.
cmake_minimum_required(VERSION 3.16)
project(meshsat-pinedio-bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

option(PINEDIO_WITH_RADIOLIB "Build what needs RadioLib: its test and the listener" ON)
option(PINEDIO_RADIOLIB_PARANOID "Let RadioLib ask for the status after every write (two more bytes, 7 ms on this bridge)" OFF)
set(RADIOLIB_DIR "" CACHE PATH "A RadioLib checkout; fetched from GitHub when empty")
set(RADIOLIB_TAG "7.7.1" CACHE STRING "RadioLib version fetched when RADIOLIB_DIR is empty")

find_package(Threads REQUIRED)

add_library(pinedio-bridge src/PineDioBridge.cpp src/LinuxI2cPort.cpp)
target_include_directories(pinedio-bridge PUBLIC src)
target_link_libraries(pinedio-bridge PUBLIC Threads::Threads)

add_library(pinedio-sim test/sim/SimBackplate.cpp)
target_include_directories(pinedio-sim PUBLIC test)
target_link_libraries(pinedio-sim PUBLIC pinedio-bridge)

enable_testing()
add_executable(test_bridge test/test_bridge.cpp)
target_link_libraries(test_bridge pinedio-sim)
add_test(NAME bridge COMMAND test_bridge)

add_executable(bridge-selftest tools/bridge-selftest/main.cpp)
target_link_libraries(bridge-selftest pinedio-bridge)

if(PINEDIO_WITH_RADIOLIB)
  if(NOT RADIOLIB_DIR)
    include(FetchContent)
    FetchContent_Declare(radiolib GIT_REPOSITORY https://github.com/jgromes/RadioLib.git GIT_TAG ${RADIOLIB_TAG} GIT_SHALLOW ON)
    FetchContent_GetProperties(radiolib)
    if(NOT radiolib_POPULATED)
      FetchContent_Populate(radiolib)
    endif()
    set(RADIOLIB_DIR ${radiolib_SOURCE_DIR})
  endif()
  # Only what an SX1262 needs, not the whole library.
  file(GLOB RADIOLIB_SOURCES
    ${RADIOLIB_DIR}/src/Module.cpp
    ${RADIOLIB_DIR}/src/Hal.cpp
    ${RADIOLIB_DIR}/src/modules/SX126x/*.cpp
    ${RADIOLIB_DIR}/src/protocols/PhysicalLayer/*.cpp
    ${RADIOLIB_DIR}/src/utils/*.cpp)
  add_library(radiolib-sx126x ${RADIOLIB_SOURCES})
  target_include_directories(radiolib-sx126x PUBLIC ${RADIOLIB_DIR}/src)
  target_compile_options(radiolib-sx126x PRIVATE -w)
  if(NOT PINEDIO_RADIOLIB_PARANOID)
    target_compile_definitions(radiolib-sx126x PUBLIC RADIOLIB_SPI_PARANOID=0)
  endif()

  add_executable(test_radiolib test/test_radiolib.cpp)
  target_link_libraries(test_radiolib pinedio-sim radiolib-sx126x)
  add_test(NAME radiolib COMMAND test_radiolib)

  add_executable(lora-listen tools/lora-listen/main.cpp)
  target_link_libraries(lora-listen pinedio-bridge radiolib-sx126x)
endif()
