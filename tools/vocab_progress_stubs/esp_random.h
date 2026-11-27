/* SPDX-License-Identifier: Apache-2.0
 * 主机测试用的随机数桩。/ Random-number stub for the host test.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t esp_random(void) { return (uint32_t)rand(); }
