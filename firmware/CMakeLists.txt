cmake_minimum_required(VERSION 3.16)

# Matter lives in this image as well as Wi-Fi (owner, 2026-09-16). One firmware, the radio chosen by a
# setting — not two downloadable builds: once the console, the access point and the updater have to work
# in Matter mode too, the only difference left between the two images was the CHIP stack itself.
#
# Build with BOTH environments sourced:
#   . $HOME/esp/v5.5.2/esp-idf/export.sh && . $HOME/esp/esp-matter/export.sh
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH is unset — source esp-matter/export.sh before building")
endif()
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Must precede project(): IDF resolves the component list while processing it.
# NOT examples/common, which the esp-matter samples all add. It brings app_reset, app_bridge and the
# rest of the sample scaffolding, and app_reset requires the managed `button` component — which this
# project does not have, so configure fails with "Failed to resolve component 'button'". Adding that
# dependency to satisfy an example we never call would be the wrong fix. The consequence: no
# common_macros.h either, so no ABORT_APP_ON_FAILURE — return codes are checked the ordinary way.
set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components")

project(smart-meteo)

# Deliberately NOT copied from the esp-matter examples: their `-std=gnu++17`, and their global `-Os`.
# This tree builds at gnu++2b and -Og, CHIP pins no standard of its own (there is no -std anywhere in
# its CMakeLists), and a scan of main/ found nothing newer than C++17 in our code — so if CHIP turns
# out to need 17, lowering it is safe, but it is not lowered on a guess. Only CHIP's own requirement
# is added here: the config header, and the warnings its generated code cannot avoid.
# The OpenThread define is esp-matter's (every Thread example sets it): Matter registers more SRP
# services than OpenThread's default buffer holds — one per fabric plus the commissionable one.
idf_build_set_property(CXX_COMPILE_OPTIONS "-DCHIP_HAVE_CONFIG_H;-DCONFIG_OPENTHREAD_CONFIG_SRP_CLIENT_BUFFERS_MAX_SERVICES=5;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-DCONFIG_OPENTHREAD_CONFIG_SRP_CLIENT_BUFFERS_MAX_SERVICES=5" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

# The Matter stack's own components ARE built the way esp-matter's examples build them, at -Os: at
# this tree's -Og the image came out at 3.21 MB against a 3 MB slot (2026-09-17). Only those four —
# our code, Wi-Fi, lwIP and mbedTLS keep the project's optimisation (owner, 2026-09-16: settings stay
# as they are). A component's PRIVATE option comes after the global -Og on its command line, so it is
# the one GCC uses; CHIP itself is compiled by GN with the flags of chip.cpp's command
# (create_args_gn.py), so the option reaches all of CHIP through the `chip` component.
# `main` joined this list on 2026-09-19, measured: the image went from 3 080 192 to 3 014 656 bytes,
# a whole 64 KB block, and the free space in the slot from 2 % to 4 %. Nothing is given up for it —
# our code is not in any hot path; the device spends about a second a minute awake and the rest
# asleep. What it did cost was three -Werror=format-truncation warnings that only -Os is thorough
# enough to find: all three were bounds GCC could not prove rather than truncations that happen, and
# they were fixed by making the bounds visible instead of by silencing the warning.
foreach(comp chip esp_matter openthread bt main)
    idf_component_get_property(lib ${comp} COMPONENT_LIB)
    target_compile_options(${lib} PRIVATE "-Os")
endforeach()
