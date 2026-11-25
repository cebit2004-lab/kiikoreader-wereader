/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：见 media_net.h。/ English: see media_net.h.
 */
#include "media_net.h"

#include <stdio.h>

#include "ai_voice.h"
#include "esp_log.h"
#include "esp_timer.h"
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
            // 自己发起的连接还在进行（如 AI 页预热被打断后）：接着等，不要拆了重来。
            // A connection we started is still in progress (say, an interrupted AI warm-up): keep
            // waiting instead of tearing it down and starting over.
            bool waiting = s_owns && st.state == READ_PICO_TRANSFER_STARTING;
            if (!waiting) {
                if (st.state != READ_PICO_TRANSFER_STOPPED) read_pico_transfer_stop();
                const read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
                waiting = read_pico_transfer_start(&cfg) == ESP_OK;
                if (waiting) s_owns = true;
            }
            if (waiting) {
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

/*
 * 旧 AI 连接的任务还在收尾时先等它退出再停 WiFi：它的套接字可能还压着驱动的接收缓冲，驱动卸载后
 * 再归还就是访问已释放的内存。它卡在 TCP 连接阶段时套接字关不掉，最长约等于连接超时（20 秒）。
 * Before stopping WiFi, wait for an old AI connection's task to exit: its socket may still hold driver
 * receive buffers, and returning them after the driver is gone touches freed memory. A task stuck in
 * the TCP connect cannot have its socket shut, so this can take up to the connect timeout (20 s).
 */
static void wait_ai_net_idle(void) {
    if (!ai_net_busy()) return;
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 250 && ai_net_busy(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (ai_net_busy()) ESP_LOGW(TAG, "old AI connection still open after %d ms; stopping WiFi anyway", ms);
    else ESP_LOGI(TAG, "waited %d ms for the old AI connection to close", ms);
}

void media_net_drop(void) {
    wait_ai_net_idle();
    take();
    read_pico_transfer_stop();
    s_owns = false;
    xSemaphoreGive(s_lock);
}

void media_net_release_if_unused(void) {
    take();
    // 旧 AI 连接还在收尾时不收：AI 的控制任务等它退出后会再调一次。
    // Not while an old AI connection is shutting down: the AI control task calls again once it exits.
    if (s_owns && !music_net_busy() && !ai_active() && !ai_net_busy()) {
        read_pico_transfer_stop();
        s_owns = false;
        ESP_LOGI(TAG, "network released");
    }
    xSemaphoreGive(s_lock);
}
