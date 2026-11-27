/* SPDX-License-Identifier: Apache-2.0
 * 主机测试用的存储桩：只有挂载 /flash，由测试决定成败。/ Storage stub for the host test: only the /flash
 * mount, whose result the test controls.
 */
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
esp_err_t book_store_flash_mount(void);
