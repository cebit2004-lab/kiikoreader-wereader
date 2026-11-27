/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * CPU 调频：空闲时降到 CPU_BOOST_IDLE_MHZ，操作、刷屏、联网、音乐和 AI 期间全速。ESP-IDF 的调频不看负载，
 * 没人持锁就一直跑最低频，所以凡是要算力的地方都要持全速锁（刷屏在显示驱动里自己持）。
 *
 * CPU frequency scaling: idle at CPU_BOOST_IDLE_MHZ, full speed while the user interacts, the screen
 * refreshes, or the network, music or the AI run. ESP-IDF scaling ignores load: with no lock held the
 * CPU stays at the minimum, so anything needing compute must hold the full-speed lock (the display
 * driver holds its own around refreshes).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_BOOST_IDLE_MHZ 80
/// 最后一次操作（或忙碌）之后保持全速这么久。/ Stay at full speed this long after the last input (or busy state).
#define CPU_BOOST_HOLD_MS 3000

/// 开机时调用一次：开启调频（不开自动浅睡）并立即持全速。/ Call once at boot: enable scaling (no automatic
/// light sleep) and hold full speed at once.
void cpu_boost_init(void);
/// 主循环调用：on 为真时持全速锁，否则放开让 CPU 降频。只在主循环任务里调用。
/// Main loop: hold the full-speed lock while on, else let the CPU slow down. Main loop task only.
void cpu_boost_set(bool on);

#ifdef __cplusplus
}
#endif
