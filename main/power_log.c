/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 电池耗电日志，见 power_log.h。采样只读电量计（BQ27220，地址 0x55）的标准命令：0x08 电压、0x0C 电流、
 * 0x10 剩余容量、0x12 满充容量、0x14 平均电流；睡眠期间不读，不碰已停放的总线。
 *
 * Battery drain log, see power_log.h. Sampling reads only the gauge's (BQ27220 at 0x55) standard
 * commands: 0x08 voltage, 0x0C current, 0x10 remaining capacity, 0x12 full-charge capacity and 0x14
 * average current. Nothing is read during sleep, so the parked bus is never touched.
 */
#include "power_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#include "metalio_hw.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#define POWER_LOG_BSS EXT_RAM_BSS_ATTR
#else
#define POWER_LOG_BSS
int64_t power_log_host_now_ms(void);
void power_log_host_read(power_log_sample_t *s);
#endif

#ifndef POWER_LOG_PATH
#define POWER_LOG_PATH "/sdcard/power_log.csv"
#define POWER_LOG_OLD_PATH "/sdcard/power_log.old.csv"
#endif
#define SAMPLE_MS 10000
#define FLUSH_MS 60000
// 每 10 秒一行约 90 字节，一天约 0.8 MB：超过 4 MB 就把旧的改名留一份，再从头写。
// One ~90-byte line every 10 s is ~0.8 MB a day: past 4 MB the old file is kept under another name.
#define FILE_MAX (4L * 1024 * 1024)
// 写不上卡（U 盘模式、没插卡）时最多攒这么多，再多就丢最早的行。
// Kept while the card cannot be written (USB drive mode, no card); beyond this the oldest lines go.
#define BUF_CAP 4096
#define LINE_MAX 256

const char power_log_header[] = "time,uptime_s,state,mv,ma,avg_ma,remain_mah,full_mah,soc,light,wifi\n";

static POWER_LOG_BSS char s_buf[BUF_CAP];
static size_t s_len;
static bool s_started;
static int64_t s_last_sample_ms, s_last_flush_ms;

static size_t put_int(char *out, size_t cap, int32_t v) {
    if (v == INT32_MIN) return snprintf(out, cap, ",");
    return snprintf(out, cap, ",%ld", (long)v);
}

size_t power_log_format(char *out, size_t cap, const power_log_sample_t *s) {
    if (!out || !s || cap < LINE_MAX) return 0;
    size_t n = 0;
    if (s->wall > 0) {
        const time_t t = (time_t)s->wall;
        struct tm tm;
        localtime_r(&t, &tm);
        n += strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
    }
    n += snprintf(out + n, cap - n, ",%lld,", (long long)(s->uptime_ms / 1000));
    // 状态取自页面标题，去掉会拆开 CSV 的字符。/ The state may be a page title: drop characters that break CSV.
    // 最多约 48 字节，截断时把当前 UTF-8 字符写完。/ About 48 bytes at most, finishing the UTF-8 character it stops in.
    const size_t t0 = n;
    for (const char *p = s->tag ? s->tag : ""; *p && (n - t0 < 48 || (*p & 0xC0) == 0x80); ++p)
        out[n++] = (*p == ',' || *p == '\n' || *p == '\r' || *p == '"') ? ' ' : *p;
    const int32_t fields[] = {s->mv, s->ma, s->avg_ma, s->remain_mah, s->full_mah, s->soc, s->light, s->wifi};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) n += put_int(out + n, cap - n, fields[i]);
    if (n + 2 > cap) return 0;
    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

#ifdef ESP_PLATFORM
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int32_t gauge(uint8_t reg, bool is_signed) {
    uint8_t r[2];
    if (metalio_read(0x55, reg, r, sizeof(r)) != ESP_OK) return INT32_MIN;
    const unsigned v = (unsigned)r[0] | (unsigned)r[1] << 8;
    return is_signed ? (int32_t)(int16_t)v : (int32_t)v;
}

static void read_sample(power_log_sample_t *s) {
    const time_t wall = time(NULL);
    s->wall = wall >= 1704067200 && wall <= 4102444799LL ? (int64_t)wall : 0;
    s->mv = gauge(0x08, false);
    s->ma = gauge(0x0C, true);
    s->remain_mah = gauge(0x10, false);
    s->full_mah = gauge(0x12, false);
    s->avg_ma = gauge(0x14, true);
    const int soc = pmu_battery_percent(read_pico_pmu_get());
    s->soc = soc < 0 ? INT32_MIN : soc;
    s->light = metalio_frontlight_enabled() ? (int32_t)metalio_brightness() : 0;
    read_pico_transfer_status_t st;
    read_pico_transfer_get_status(&st);
    s->wifi = st.state != READ_PICO_TRANSFER_STOPPED && st.url[0] != '\0';
}
#else
static int64_t now_ms(void) { return power_log_host_now_ms(); }
static void read_sample(power_log_sample_t *s) { power_log_host_read(s); }
#endif

static void append(const char *tag, int64_t now) {
    power_log_sample_t s = {.uptime_ms = now, .tag = tag};
    read_sample(&s);
    char line[LINE_MAX];
    const size_t n = power_log_format(line, sizeof(line), &s);
    if (!n) return;
    // 放不下就丢最早的整行。/ Drop the oldest whole lines until it fits.
    while (s_len + n > BUF_CAP) {
        const char *nl = memchr(s_buf, '\n', s_len);
        const size_t drop = nl ? (size_t)(nl - s_buf) + 1 : s_len;
        memmove(s_buf, s_buf + drop, s_len - drop);
        s_len -= drop;
    }
    memcpy(s_buf + s_len, line, n);
    s_len += n;
}

static void flush(void) {
    if (!s_len) return;
    struct stat st;
    bool fresh = stat(POWER_LOG_PATH, &st) != 0 || st.st_size == 0;
    if (!fresh && st.st_size > FILE_MAX) {
        remove(POWER_LOG_OLD_PATH);
        fresh = rename(POWER_LOG_PATH, POWER_LOG_OLD_PATH) == 0;
    }
    FILE *f = fopen(POWER_LOG_PATH, "a");
    if (!f) return;  // 卡不可写：留着下次再写 / card not writable: keep for next time
    const bool ok = (!fresh || fputs(power_log_header, f) >= 0) && fwrite(s_buf, 1, s_len, f) == s_len;
    if (fclose(f) == 0 && ok) s_len = 0;
}

void power_log_tick(const char *tag) {
    const int64_t now = now_ms();
    if (!s_started) {
        power_log_mark("boot");
        return;
    }
    if (now - s_last_sample_ms >= SAMPLE_MS) {
        s_last_sample_ms = now;
        append(tag, now);
    }
    if (s_len && now - s_last_flush_ms >= FLUSH_MS) {
        s_last_flush_ms = now;
        flush();
    }
}

void power_log_mark(const char *event) {
    const int64_t now = now_ms();
    s_started = true;
    s_last_sample_ms = s_last_flush_ms = now;
    append(event, now);
    flush();
}
