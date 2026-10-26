set(board_sources "jjysim_board.c")
if(IDF_TARGET STREQUAL "esp32c3")
    list(APPEND board_sources "esp_efuse_custom_table.c")
endif()

idf_component_register(SRCS "ota_update.c" "ntp_server.c" "ntp_server_packet.c" "storage_crypto.c" "led.c" "jjy_pwm.c" "main.c" "settings.c" "wifi_mng.c" ${board_sources}
                    INCLUDE_DIRS "include")
