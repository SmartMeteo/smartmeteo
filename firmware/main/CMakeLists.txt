# Bosch's BSEC turns the BME690's gas resistance into the air-quality index. It is closed, and its
# licence forbids handing the library, headers or configuration to anyone else, so the public source
# does not carry them: components/bsec2/README.md says where to get them. With all four files there
# the gas manager is the real one; with none of them it is bsec_manager_none.cpp, and the device works
# without an index. Some but not all is a mistake made while adding them, and stops the build.
set(BSEC_DIR "${CMAKE_CURRENT_LIST_DIR}/../components/bsec2")
set(BSEC_FILES lib/libalgobsec.a include/bsec_interface.h include/bsec_datatypes.h config/bsec_iaq.config)
set(BSEC_MISSING "")
foreach(f ${BSEC_FILES})
    if(NOT EXISTS "${BSEC_DIR}/${f}")
        list(APPEND BSEC_MISSING "${f}")
    endif()
endforeach()
list(LENGTH BSEC_FILES BSEC_ALL)
list(LENGTH BSEC_MISSING BSEC_GONE)
if(BSEC_GONE EQUAL 0)
    set(GAS_SRC "bsec_manager.cpp")
elseif(BSEC_GONE EQUAL BSEC_ALL)
    set(GAS_SRC "bsec_manager_none.cpp")
    message(STATUS "Bosch BSEC not in components/bsec2: building WITHOUT the air-quality index "
                   "(components/bsec2/README.md says how to add it)")
else()
    message(FATAL_ERROR "components/bsec2 is incomplete — missing: ${BSEC_MISSING}. "
                        "Add all four files (components/bsec2/README.md) or none of them.")
endif()

idf_component_register(SRCS "main.cpp" "pm_gate.cpp" "telemetry.cpp" "panel_bench.cpp" "border_bench.cpp" "scd_bench.cpp" "hist_bench.cpp" "board_bench.cpp" "bme_probe.cpp" "curve_bench.cpp" "curve_log.cpp" "curve_summary.cpp" "curve_screen.cpp" "curve_page.cpp" "bench_screens.cpp" "i2c_manager.cpp" "max17048.cpp" "battery_monitor.cpp" "battery_display.cpp" "power_estimate.cpp" "power_history.cpp"
                    "battery_source.cpp" "scd41.cpp" "bme690.cpp" "bme69x.c" "epaper.cpp" "epd_vendor.cpp" "screen.cpp"
    "sun_moon.cpp" "nvs_manager.cpp" "analytics.cpp" "wifi_manager.cpp" "wifi_slots.cpp" "web_server.cpp" "${GAS_SRC}" "clock_sync.cpp" "captive_dns.cpp" "weather.cpp" "https_client.cpp" "hist_store.cpp" "telegram.cpp" "mqtt_publisher.cpp" "ota_check.cpp" "ota_install.cpp" "evt_log.cpp" "matter_mode.cpp" "qrcodegen.c" "portal_screen.cpp" "portal_art.cpp"
                       # ArduinoJson lives in components/ArduinoJson and is picked up on its own:
                       # main implicitly depends on every component in the project, and naming one
                       # explicitly here would replace that implicit set with just the named entry —
                       # which loses esp_timer, the I2C driver and everything else in one line.
                       INCLUDE_DIRS ".")

# The console is 236 KB of text and it is served for a few minutes a month. Compressed at build
# time it is 72 KB — 163 KB of flash back for the cost of one HTTP header, and every browser has
# understood Content-Encoding: gzip for thirty years. Generated into the build directory rather
# than committed, so the served page can never drift from the source it was made from.
set(CONSOLE_GZ "${CMAKE_CURRENT_BINARY_DIR}/console.html.gz")
add_custom_command(
    OUTPUT "${CONSOLE_GZ}"
    COMMAND ${CMAKE_COMMAND} -E echo "gzip console.html -> console.html.gz"
    COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/gzip_console.py" "${CMAKE_CURRENT_LIST_DIR}/console.html" "${CONSOLE_GZ}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/console.html" "${CMAKE_CURRENT_LIST_DIR}/gzip_console.py"
    VERBATIM)
add_custom_target(console_gz_target DEPENDS "${CONSOLE_GZ}")
add_dependencies(${COMPONENT_TARGET} console_gz_target)
target_add_binary_data(${COMPONENT_TARGET} "${CONSOLE_GZ}" BINARY)
target_add_binary_data(${COMPONENT_TARGET} "app_icon.png" BINARY)   # home-screen icon
if(GAS_SRC STREQUAL "bsec_manager.cpp")
    # ULP 300 s (battery). Bosch's bme690_iaq_33v_300s_4d; the symbol comes from the file name, not
    # its folder, so moving it out of main/ changed nothing the code refers to.
    target_add_binary_data(${COMPONENT_TARGET} "${BSEC_DIR}/config/bsec_iaq.config" BINARY)
endif()

# The setup screen is a pre-rendered 4-gray picture (portal_art.h, from Software/mockups/portal_art.py)
# with the QR code and the password line left blank: portal_screen.cpp draws both for the password
# this device drew for itself (WiFiManager::prepareApPassword). The QR encoder is qrcodegen.c, the
# same file the Matter pairing screen uses. The password is random, not derived from the MAC: the MAC
# is on the air as the access point's BSSID, so anything computed from it can be computed by anyone.
