/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：音乐与 AI 语音共用的联网管理。只连接已保存的 WiFi；本模块启动的连接由本模块在两者都不用时收回。
 * 会启动/停止 WiFi（读写 NVS、关缓存），只能从栈在内部 RAM 的任务调用，不能用 PSRAM 栈。
 *
 * English: Network ownership shared by music and AI voice. Joins the saved WiFi only; a connection this
 * module started is released once neither needs it. Starting/stopping WiFi touches NVS with the cache
 * off, so call only from tasks whose stack is internal RAM, never a PSRAM stack.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 需要时连上已保存的 WiFi（最多约 30 秒）；失败写 err。cancel 非空且变为真时立即放弃。
/// Join the saved WiFi if needed (up to ~30 s); writes err on failure. Gives up at once when a non-NULL cancel turns true.
bool media_net_acquire(char *err, size_t err_size, const volatile bool *cancel);
/// 断开本模块启动的 WiFi（用于先占 I2S 再重连）。/ Drop WiFi this module started (to claim I2S first, then reconnect).
void media_net_drop(void);
/// 音乐与 AI 都不再需要时断开本模块启动的 WiFi。/ Drop WiFi this module started once neither music nor AI needs it.
void media_net_release_if_unused(void);

#ifdef __cplusplus
}
#endif
