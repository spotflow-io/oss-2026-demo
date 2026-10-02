# Copyright (c) 2026 Spotflow s.r.o.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

if(APP_CREDENTIALS_FILE)
	if(NOT IS_ABSOLUTE "${APP_CREDENTIALS_FILE}")
		set(APP_CREDENTIALS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${APP_CREDENTIALS_FILE}")
	endif()
	if(NOT EXISTS "${APP_CREDENTIALS_FILE}")
		message(FATAL_ERROR "APP_CREDENTIALS_FILE does not exist: ${APP_CREDENTIALS_FILE}")
	endif()
	list(APPEND EXTRA_CONF_FILE "${APP_CREDENTIALS_FILE}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(asset_tracker)

if(CONFIG_SPOTFLOW_TRANSPORT_MQTT)
	if("${CONFIG_APP_WIFI_SSID}" STREQUAL "")
		message(FATAL_ERROR "CONFIG_APP_WIFI_SSID must be set for the MQTT variant")
	endif()
	if("${CONFIG_APP_WIFI_PASSWORD}" STREQUAL "")
		message(FATAL_ERROR "CONFIG_APP_WIFI_PASSWORD must be set for the MQTT variant")
	endif()
	if("${CONFIG_SPOTFLOW_INGEST_KEY}" STREQUAL "")
		message(FATAL_ERROR "CONFIG_SPOTFLOW_INGEST_KEY must be set for the MQTT variant")
	endif()
endif()

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

target_sources_ifdef(CONFIG_SPOTFLOW_TRANSPORT_MQTT app PRIVATE src/wifi_link.c)
