/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * CPU 调频，见 cpu_boost.h。/ CPU frequency scaling, see cpu_boost.h.
 */
#include "cpu_boost.h"

#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_log.h"
#include "esp_pm.h"

static const char *TAG = "cpu_boost";
static esp_pm_lock_handle_t s_lock;
static bool s_held;

void cpu_boost_init(void) {
    if (s_lock) return;
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "cpu_boost", &s_lock) != ESP_OK) {
        s_lock = NULL;
        ESP_LOGW(TAG, "no lock: scaling left off");
        return;
    }
    // 先持锁再开调频，开机过程一直全速。/ Hold before enabling scaling so boot runs at full speed throughout.
    cpu_boost_set(true);
    const esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CPU_BOOST_IDLE_MHZ,
        .light_sleep_enable = false,
    };
    const esp_err_t err = esp_pm_configure(&cfg);
    ESP_LOGI(TAG, "scaling %d-%d MHz: %s", CPU_BOOST_IDLE_MHZ, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_err_to_name(err));
}

void cpu_boost_set(bool on) {
    if (!s_lock || on == s_held) return;
    if ((on ? esp_pm_lock_acquire(s_lock) : esp_pm_lock_release(s_lock)) == ESP_OK) s_held = on;
}
#else
void cpu_boost_init(void) {}
void cpu_boost_set(bool on) { (void)on; }
#endif
