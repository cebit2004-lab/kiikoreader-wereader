/* SPDX-License-Identifier: Apache-2.0
 * 主机测试用的日志桩：设 VOCAB_TEST_LOG=1 才打印。/ Log stub for the host test; prints only with VOCAB_TEST_LOG=1.
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
static inline int vocab_test_log_on(void) { const char *v = getenv("VOCAB_TEST_LOG"); return v && *v == '1'; }
#define VOCAB_TEST_LOG_(lvl, tag, fmt, ...) \
    do { if (vocab_test_log_on()) fprintf(stderr, lvl " %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) VOCAB_TEST_LOG_("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) VOCAB_TEST_LOG_("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) VOCAB_TEST_LOG_("I", tag, fmt, ##__VA_ARGS__)
