# Bosch's BSEC: the closed library, its two headers and the configuration for this sensor. They are
# not ours to publish (licence, December 2024, §3.1), so the public source has only this file and
# README.md here, and the component is then empty. main/CMakeLists.txt sees the same files and builds
# bsec_manager_none.cpp instead of bsec_manager.cpp; it also stops the build when only some of them
# are present, because a half-added BSEC quietly building without the index would be worse.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/libalgobsec.a"
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/include/bsec_interface.h")
    idf_component_register(
        INCLUDE_DIRS "include"
    )

    add_library(algobsec STATIC IMPORTED)
    set_property(TARGET algobsec PROPERTY IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/lib/libalgobsec.a")
    target_link_libraries(${COMPONENT_TARGET} INTERFACE algobsec)
else()
    idf_component_register()
endif()
