/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：不编入蓝牙的固件用的空实现（CONFIG_BT_ENABLED 未开）。接口与 ble_page_turner.c 相同，调用方不用改：
 * 栈永远不启动、没有设备和配对、没有按键；联网前的「释放蓝牙」直接成功。真实现仍在 ble_page_turner.c，
 * 打开蓝牙配置后 CMake 会改回编它。
 *
 * English: Stub for firmware built without Bluetooth (CONFIG_BT_ENABLED off). Same interface as
 * ble_page_turner.c, so callers need no changes: the stack never starts, there are no devices,
 * bonds or keys, and "release Bluetooth before networking" succeeds at once. The real
 * implementation stays in ble_page_turner.c; CMake switches back to it once Bluetooth is enabled.
 */
#include "ble_page_turner.h"

#include <string.h>

static void no_reason(char *out, size_t cap) {
    if (out && cap) out[0] = 0;
}

esp_err_t ble_pt_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void ble_pt_sync(bool wanted) { (void)wanted; }
bool ble_pt_take_start_failure(char *out, size_t cap) { no_reason(out, cap); return false; }
void ble_pt_reset_failure(void) {}
esp_err_t ble_pt_stop(uint32_t timeout_ms) { (void)timeout_ms; return ESP_OK; }
esp_err_t ble_pt_suspend_for_sleep(void) { return ESP_OK; }
void ble_pt_resume_after_sleep(void) {}
bool ble_pt_running(void) { return false; }
// 没有蓝牙要让出：WiFi 直接继续。/ There is no Bluetooth to stop: WiFi goes ahead at once.
esp_err_t ble_pt_network_acquire(void) { return ESP_OK; }
void ble_pt_network_release(void) {}
void ble_pt_poll(void) {}
uint32_t ble_pt_input_serial(void) { return 0; }

void ble_pt_scan_start(uint32_t ms) { (void)ms; }
void ble_pt_scan_stop(void) {}
bool ble_pt_scanning(void) { return false; }
uint8_t ble_pt_device_count(void) { return 0; }
const ble_pt_device_t *ble_pt_device(uint8_t index) { (void)index; return NULL; }

esp_err_t ble_pt_connect(const char *addr) { (void)addr; return ESP_ERR_NOT_SUPPORTED; }
void ble_pt_reconnect_recent(void) {}
void ble_pt_cancel_recent(void) {}
void ble_pt_disconnect(void) {}
bool ble_pt_connected(void) { return false; }
bool ble_pt_connecting(void) { return false; }
const char *ble_pt_connected_name(void) { return ""; }
bool ble_pt_take_failure(char *out, size_t cap) { no_reason(out, cap); return false; }

void ble_pt_crumb(uint8_t id) { (void)id; }
bool ble_pt_take_passkey(uint32_t *out) { (void)out; return false; }

uint8_t ble_pt_bond_count(void) { return 0; }
const ble_pt_bond_t *ble_pt_bond(uint8_t index) { (void)index; return NULL; }
esp_err_t ble_pt_forget(const char *addr) { (void)addr; return ESP_ERR_NOT_SUPPORTED; }

bool ble_pt_pop_key(ble_pt_event_t *out) { (void)out; return false; }
bool ble_pt_pop_raw(ble_pt_raw_t *out) { (void)out; return false; }

void ble_pt_learn_begin(void) {}
esp_err_t ble_pt_bind(ble_pt_action_t action, const ble_pt_frame_t *frame) {
    (void)action; (void)frame;
    return ESP_ERR_NOT_SUPPORTED;
}
bool ble_pt_binding(ble_pt_action_t action, ble_pt_frame_t *out) { (void)action; (void)out; return false; }
ble_pt_action_t ble_pt_action_for_frame(const ble_pt_frame_t *frame) { (void)frame; return BLE_PT_ACTION_NONE; }
ble_pt_action_t ble_pt_action_for_usage(uint8_t usage, uint8_t mods) {
    (void)usage; (void)mods;
    return BLE_PT_ACTION_NONE;
}
bool ble_pt_take_standby(void) { return false; }
