include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake")
# BLE HID 主机（蓝牙翻页器）。依赖 NimBLE 主机与 BT 控制器，都是 IDF 自带组件。
# 没编入蓝牙的固件（CONFIG_BT_ENABLED 未开，目前是 metalio47）换成空实现，接口不变。
# BLE HID host (Bluetooth page-turner). Needs the NimBLE host and the BT controller, both
# bundled with ESP-IDF. Firmware built without Bluetooth (CONFIG_BT_ENABLED off, currently
# metalio47) gets a stub with the same interface.
if(CONFIG_BT_ENABLED)
    kiiko_board_sources(kiiko_selected_sources "${CMAKE_CURRENT_LIST_DIR}" "src/ble_page_turner.c")
else()
    set(kiiko_selected_sources "src/ble_page_turner_stub.c")
endif()
idf_component_register(
    SRCS ${kiiko_selected_sources}
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_timer read_pico
)
