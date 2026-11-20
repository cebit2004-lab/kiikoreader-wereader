/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：见 media_net.h。/ English: see media_net.h.
 */
#include "media_net.h"

#include <stdio.h>

#include "ai_voice.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music_player.h"
#include "read_pico_transfer.h"

static const char *TAG = "media_net";
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static bool s_owns;

static void take(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

bool media_net_acquire(char *err, size_t err_size, const volatile bool *cancel) {
    take();
    read_pico_transfer_status_t st;
    read_pico_transfer_get_status(&st);
    bool ok = st.network_ready;
    if (!ok) {
        char ssid[33];
        bool configured = false;
        read_pico_transfer_get_saved_wifi(ssid, &configured);
        if (!configured) {
            snprintf(err, err_size, "请先在设置里连接 WiFi");
        } else {
            if (st.state != READ_PICO_TRANSFER_STOPPED) read_pico_transfer_stop();
            const read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
            if (read_pico_transfer_start(&cfg) == ESP_OK) {
                s_owns = true;
                for (int i = 0; i < 300 && !ok && !(cancel && *cancel); ++i) {
                    read_pico_transfer_service_poll();
                    read_pico_transfer_get_status(&st);
                    ok = st.network_ready;
                    if (!ok) vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            if (!ok) snprintf(err, err_size, cancel && *cancel ? "已取消" : "WiFi 连接失败");
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void media_net_drop(void) {
    take();
    read_pico_transfer_stop();
    s_owns = false;
    xSemaphoreGive(s_lock);
}

void media_net_release_if_unused(void) {
    take();
    if (s_owns && !music_net_busy() && !ai_active()) {
        read_pico_transfer_stop();
        s_owns = false;
        ESP_LOGI(TAG, "network released");
    }
    xSemaphoreGive(s_lock);
}
