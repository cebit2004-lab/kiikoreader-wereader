/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 耗电日志：行格式、10 秒采样与每分钟写卡、写不上卡时攒着、锁屏/解锁立即写、超限改名。
 * Drain log: line format, 10 s sampling with a write each minute, keeping lines while the card is not
 * writable, immediate writes on lock/unlock, and renaming past the size limit.
 */
#include "power_log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t s_now;
static int s_reads;
int64_t power_log_host_now_ms(void) { return s_now; }
void power_log_host_read(power_log_sample_t *s) {
    ++s_reads;
    s->wall = 0;
    s->mv = 3900;
    s->ma = -42;
    s->avg_ma = -40;
    s->remain_mah = 1500 - s_reads;
    s->full_mah = INT32_MIN;  // 读不到 / unreadable
    s->soc = 80;
    s->light = 0;
    s->wifi = 1;
}

static char *slurp(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { *size = -1; return NULL; }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    rewind(f);
    char *buf = calloc(1, (size_t)*size + 1);
    assert(fread(buf, 1, (size_t)*size, f) == (size_t)*size);
    fclose(f);
    return buf;
}

static int count(const char *text, const char *needle) {
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) ++n;
    return n;
}

int main(void) {
    // 行格式：空的时间、缺的字段留空，页面标题里的逗号换成空格。
    // Line format: empty time and missing fields stay blank; commas in a page title become spaces.
    char line[256];
    power_log_sample_t s = {.uptime_ms = 12345, .tag = "书架,首页", .mv = 3900, .ma = -42, .avg_ma = -40,
                            .remain_mah = 1500, .full_mah = INT32_MIN, .soc = 80, .light = 120, .wifi = 0};
    size_t n = power_log_format(line, sizeof(line), &s);
    assert(n == strlen(line));
    assert(!strcmp(line, ",12,书架 首页,3900,-42,-40,1500,,80,120,0\n"));
    assert(power_log_format(line, 64, &s) == 0);
    // 很长的标题截在 UTF-8 字符边界上。/ A long title is cut on a UTF-8 character boundary.
    s.tag = "一二三四五六七八九十一二三四五六七八九十";
    n = power_log_format(line, sizeof(line), &s);
    assert(strstr(line, ",一二三四五六七八九十一二三四五六,"));

    remove(POWER_LOG_PATH);
    remove(POWER_LOG_OLD_PATH);
    long size;
    char *text;

    // 第一次 tick 记开机并立即写卡，带表头。/ The first tick logs boot and writes at once, with the header.
    s_now = 1000;
    power_log_tick("read");
    text = slurp(POWER_LOG_PATH, &size);
    assert(text && !strncmp(text, power_log_header, strlen(power_log_header)));
    assert(count(text, ",boot,") == 1);
    free(text);

    // 10 秒采一次，一分钟内不写卡。/ A sample every 10 s, no card write within the minute.
    for (s_now = 1000; s_now < 60000; s_now += 500) power_log_tick("read");
    text = slurp(POWER_LOG_PATH, &size);
    assert(count(text, ",read,") == 0);
    free(text);
    s_now = 61000;
    power_log_tick("music");
    text = slurp(POWER_LOG_PATH, &size);
    assert(count(text, ",read,") == 5 && count(text, ",music,") == 1);
    assert(count(text, "time,uptime_s") == 1);
    free(text);

    // 锁屏、解锁立即写。/ Lock and unlock write at once.
    s_now = 65000;
    power_log_mark("lock");
    s_now = 665000;
    power_log_mark("wake");
    text = slurp(POWER_LOG_PATH, &size);
    assert(count(text, ",lock,") == 1 && count(text, ",wake,") == 1);
    assert(strstr(text, ",665,wake,3900,-42,-40,"));
    free(text);

    // 写不上卡时攒着，能写时一次补上。/ Kept while the card is not writable, then written in one go.
    remove(POWER_LOG_PATH);
    assert(mkdir(POWER_LOG_PATH, 0700) == 0);  // 目录占位，fopen 失败 / a directory in the way makes fopen fail
    for (int i = 0; i < 30; ++i) { s_now += 10000; power_log_tick("ai"); }
    assert(rmdir(POWER_LOG_PATH) == 0);
    s_now += 60000;
    power_log_tick("ai");
    text = slurp(POWER_LOG_PATH, &size);
    assert(count(text, ",ai,") == 31);
    free(text);

    // 一直写不上卡：只留最近的行，每行完整。/ Card never writable: only recent lines are kept, each whole.
    remove(POWER_LOG_PATH);
    assert(mkdir(POWER_LOG_PATH, 0700) == 0);
    for (int i = 0; i < 400; ++i) { s_now += 10000; power_log_tick("lost"); }
    assert(rmdir(POWER_LOG_PATH) == 0);
    power_log_mark("wake");
    text = slurp(POWER_LOG_PATH, &size);
    const int kept = count(text, ",lost,");
    assert(kept > 10 && kept < 400 && size <= 4096 + (long)strlen(power_log_header));
    assert(!strncmp(text, power_log_header, strlen(power_log_header)));
    assert(text[size - 1] == '\n' && count(text, ",wake,") == 1);
    free(text);

    // 超过上限把旧文件改名，新文件重新带表头。/ Past the limit the old file is renamed and the new one gets a header.
    FILE *f = fopen(POWER_LOG_PATH, "a");
    assert(f);
    static char filler[1024 * 1024];
    memset(filler, 'x', sizeof(filler));
    for (int i = 0; i < 5; ++i) assert(fwrite(filler, 1, sizeof(filler), f) == sizeof(filler));
    fclose(f);
    power_log_mark("boot");
    text = slurp(POWER_LOG_PATH, &size);
    assert(!strncmp(text, power_log_header, strlen(power_log_header)) && count(text, ",boot,") == 1);
    assert(size < 1024);
    free(text);
    struct stat st;
    assert(stat(POWER_LOG_OLD_PATH, &st) == 0 && st.st_size > 5 * 1024 * 1024);

    remove(POWER_LOG_PATH);
    remove(POWER_LOG_OLD_PATH);
    puts("PASS: power log format, sampling, buffering, lock/wake marks and rotation");
    return 0;
}
