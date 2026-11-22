# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pmbus_target)

set(PMBUS_LIB ${CMAKE_CURRENT_SOURCE_DIR}/../lib/pmbus)

target_include_directories(app PRIVATE ${PMBUS_LIB}/include)
target_sources(app PRIVATE
  ${PMBUS_LIB}/src/device.c
  ${PMBUS_LIB}/src/format.c
  ${PMBUS_LIB}/src/target.c
  src/main.c
  src/pmbus_i2c.c
  src/storage.c
  src/shell_cmds.c
)
