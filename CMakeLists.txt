cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(coreS3)

# Validate both the static layout and the runtime heap reservation of the
# modem's snapshot bank in the firmware that will actually be exported.
idf_build_get_property(SDR_CHECK_PYTHON PYTHON)
add_custom_target(check_sdr_sram ALL
    COMMAND "${SDR_CHECK_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/check_sram.py"
            "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf"
    DEPENDS "${CMAKE_PROJECT_NAME}.elf"
    COMMENT "Checking SDR snapshot SRAM reservation"
    VERBATIM
)

# ===== POST-BUILD: copy binaries to binaries-esp32s3 =====
set(EXPORT_DIR "${CMAKE_SOURCE_DIR}/binaries-esp32s3")
file(MAKE_DIRECTORY "${EXPORT_DIR}")

set(COPY_STAMP "${CMAKE_BINARY_DIR}/copy_bins.stamp")
set(GEN_APP_BIN_TARGET "gen_${CMAKE_PROJECT_NAME}_binary")
set(_copy_base_dep "")
if(TARGET ${GEN_APP_BIN_TARGET})
  set(_copy_base_dep ${GEN_APP_BIN_TARGET})
elseif(TARGET app)
  set(_copy_base_dep app)
else()
  message(WARNING "copy_firmware: no binary generation target found; disabling auto copy")
endif()

if(_copy_base_dep)
  set(COPY_DEPS ${_copy_base_dep})

  add_custom_command(
    OUTPUT "${COPY_STAMP}"
    COMMAND ${CMAKE_COMMAND}
            -DDEST="${EXPORT_DIR}"
            -DBLD="${CMAKE_BINARY_DIR}"
            -DAPP="${PROJECT_NAME}.bin"
            -P "${CMAKE_SOURCE_DIR}/post_build.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${COPY_STAMP}"
    # ESP-IDF declares the binary timestamp as its build output; the .bin
    # itself has no Ninja rule on a clean build.
    DEPENDS ${COPY_DEPS}
            "${CMAKE_BINARY_DIR}/.bin_timestamp"
            "${CMAKE_SOURCE_DIR}/post_build.cmake"
    COMMENT "Copying firmware binaries to ${EXPORT_DIR}"
  )

  add_custom_target(copy_firmware ALL DEPENDS "${COPY_STAMP}")
  add_dependencies(copy_firmware ${COPY_DEPS})
  add_dependencies(copy_firmware check_sdr_sram)
endif()
