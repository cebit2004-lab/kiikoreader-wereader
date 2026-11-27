/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 电池耗电日志：醒着时每 10 秒读一次电量计，每分钟追加到 TF 卡上的 CSV；锁屏、解锁和开机各记一行，
 * 用两行之间的剩余容量差算睡眠和关机期间的平均电流。只在主循环任务里调用。
 *
 * Battery drain log: while awake, sample the gauge every 10 s and append to a CSV on the card once a
 * minute; lock, unlock and boot each add a line, so the remaining-capacity difference between two lines
 * gives the average current during sleep or power-off. Call from the main loop task only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 一次采样。读不到的项为 INT32_MIN。/ One sample. Fields that could not be read are INT32_MIN.
typedef struct {
    int64_t wall;        ///< 墙上时间（秒），时间无效时为 0 / Wall time (s), 0 when the clock is not valid
    int64_t uptime_ms;   ///< 本次开机以来 / Since this boot
    const char *tag;     ///< 状态或事件 / State or event
    int32_t mv;          ///< 电压 / Voltage
    int32_t ma;          ///< 瞬时电流（放电为负）/ Instant current (negative when discharging)
    int32_t avg_ma;      ///< 电量计的平均电流 / The gauge's average current
    int32_t remain_mah;  ///< 剩余容量 / Remaining capacity
    int32_t full_mah;    ///< 满充容量 / Full-charge capacity
    int32_t soc;         ///< 状态栏显示的百分比 / Percentage shown in the status bar
    int32_t light;       ///< 前光亮度，关着为 0 / Frontlight brightness, 0 when off
    int32_t wifi;        ///< 已连上 WiFi 为 1 / 1 while WiFi is connected
} power_log_sample_t;

/// CSV 表头（含换行）。/ The CSV header line, newline included.
extern const char power_log_header[];

/// 把一次采样写成一行 CSV（含换行），返回长度；放不下返回 0。
/// Format one sample as a CSV line, newline included; returns its length, or 0 if it does not fit.
size_t power_log_format(char *out, size_t cap, const power_log_sample_t *s);

/// 主循环每圈调用。tag 是当前状态（如 "read"、"music"、"ai"）；内部每 10 秒采一次、每分钟写一次卡。
/// Call on every main-loop pass. tag is the current state ("read", "music", "ai", ...); samples every
/// 10 s and writes the card once a minute.
void power_log_tick(const char *tag);

/// 立即采一次并写卡（锁屏前、解锁后、开机）。/ Sample and write the card now (before lock, after unlock, at boot).
void power_log_mark(const char *event);

#ifdef __cplusplus
}
#endif
