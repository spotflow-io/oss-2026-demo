# Copyright (c) 2026 Spotflow s.r.o.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(asset_tracker)

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
