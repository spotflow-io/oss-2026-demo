# Copyright (c) 2026 Spotflow s.r.o.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The RAM profile is the same for every CC23x0 board, so it lives in one file rather than
# in each board's fragment. It has to be named here because Kconfig fragments cannot
# include one another; anything genuinely board-specific still goes in boards/<board>.conf,
# which Zephyr picks up on its own. Merge order is prj.conf, then the board fragment, then
# this - and the sets do not overlap, so the order costs nothing.
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/cc23x0.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cc2340r5_asset_tracker)

if(CONFIG_APP_SENSOR_BMI270)
	# The 8 KiB configuration blob the part needs at every boot ships with the
	# Zephyr driver; we use the blob but drive the part ourselves.
	target_include_directories(app PRIVATE
		${ZEPHYR_BASE}/drivers/sensor/bosch/bmi270)
	target_sources(app PRIVATE src/sensor_bmi270.c)
else()
	target_sources(app PRIVATE src/sensor_sim.c)
endif()

target_sources(app PRIVATE
	src/main.c
	src/tracker.c
	src/diag_metrics.c
	src/link_monitor.c
	src/power_model.c
	src/geo_sim.c
	src/faults.c
	src/session_meta.c
	src/boot_info.c
	src/shake_detect.c
)
