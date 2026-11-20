include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake")
if(NOT KIIKO_BOARD_METALIO47)
    idf_component_register()
    return()
endif()
idf_component_register(SRCS "metalio_audio.c" INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_driver_uart
    PRIV_REQUIRES metalio_hw esp_timer freertos log heap)
