/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
// 无操作计时；网络及升级后台不算用户操作。/ Idle time excludes background network and update ticks.
// Metalio47：最短一档存的值仍是 1（设置与备份不变），时长改为 90 秒。
// Metalio47: the shortest option is still stored as 1 (settings and backups unchanged) but lasts 90 s.
static inline int64_t auto_lock_ms(unsigned minutes) {
#if KIIKO_BOARD_METALIO47
    if (minutes == 1) return 90000;
#endif
    return (int64_t)minutes * 60000;
}
static inline bool auto_lock_due(int64_t now_ms, int64_t last_input_ms, unsigned minutes) {
    return (minutes == 1 || minutes == 5 || minutes == 10) && now_ms >= last_input_ms &&
           now_ms - last_input_ms >= auto_lock_ms(minutes);
}
