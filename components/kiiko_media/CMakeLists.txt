include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake")
# minimp3.h：CC0-1.0，来自 github.com/lieff/minimp3，许可见 LICENSE.minimp3。
# minimp3.h is CC0-1.0 from github.com/lieff/minimp3; see LICENSE.minimp3.
idf_component_register(SRCS "music_player.c" "media_net.c" "ai_voice.c" "word_voice.c" INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES metalio_audio freertos log heap esp_hw_support esp_http_client mbedtls
                  espressif__cjson read_pico_transfer esp_timer espressif__esp_websocket_client esp_wifi
                  esp_netif esp_event lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function)
