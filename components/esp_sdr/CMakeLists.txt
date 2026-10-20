if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "This RF capture backend requires ESP32-S3")
endif()
idf_component_register(
    SRCS "receiver.c" "upstream/rx_recalibration.c" "upstream/rx_calibration_state.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "upstream"
    REQUIRES esp_wifi esp_phy esp_event esp_timer esp_hw_support heap)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/sram_guard.ld"
    "-Wl,--wrap=chip_v7_set_chan_ana"
    "-Wl,--wrap=set_rx_gain_cal_dc")
