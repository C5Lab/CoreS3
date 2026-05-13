idf_component_register(
    SRCS
        "main.c"
        "uart_handler.c"
        "parse_worker.c"
        "ui_helpers.c"
        "lvgl_psram_malloc.c"
        "psram_dynarr.c"
        "cardkb.c"
        "subghz_parser.c"
        "led_indicator.c"
        "screens/home_screen.c"
        "screens/wifi_scan_screen.c"
        "screens/attack_select_screen.c"
        "screens/evil_twin_screen.c"
        "screens/keyboard_screen.c"
        "screens/global_attacks_screen.c"
        "screens/handshaker_screen.c"
        "screens/portal_screen.c"
        "screens/wardrive_screen.c"
        "screens/sae_overflow_screen.c"
        "screens/sniffer_screen.c"
        "screens/karma_screen.c"
        "screens/network_observer_screen.c"
        "screens/compromised_data_screen.c"
        "screens/bluetooth_screen.c"
        "screens/deauth_detector_screen.c"
        "screens/subghz_screen.c"
        "screens/subghz_listen_screen.c"
        "screens/subghz_transmit_screen.c"
        "screens/subghz_manage_screen.c"
        "screens/subghz_jammer_screen.c"
        "screens/subghz_tesla_screen.c"
        "screens/subghz_hunter_screen.c"
    INCLUDE_DIRS
        "."
        "screens"
    REQUIRES
        driver
        esp_driver_uart
        esp_driver_gpio
        esp_driver_i2c
        esp_timer
        nvs_flash
        esp_lvgl_port
        esp_codec_dev
        m5stack_core_s3
        lvgl__lvgl
        espressif__led_strip
)

# Force inclusion of our LVGL custom-malloc backend so the linker pulls
# the lv_*_core symbols in even though they're called from inside LVGL,
# which is scanned before main.a in the one-pass link.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u lv_malloc_core"
    "-u lv_realloc_core"
    "-u lv_free_core"
    "-u lv_mem_init"
    "-u lv_mem_deinit"
    "-u lv_mem_monitor_core"
    "-u lv_mem_test_core"
    "-u lv_mem_add_pool"
    "-u lv_mem_remove_pool"
)
