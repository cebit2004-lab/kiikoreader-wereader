/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：崩溃现场的读取接口。记录由 panic 包装器写进 RTC（带出事固件的 ELF 摘要），开机时打到串口并
 * 追加到 TF 卡上的 crash_log.txt，写成后清除。
 *
 * English: Reading side of the panic scene. The panic wrapper writes it into RTC (with the ELF digest
 * of the build that crashed); at boot it is logged and appended to crash_log.txt on the card, then
 * cleared once written.
 */

#pragma once

#include <stdbool.h>

/// 是否有上一次崩溃的记录。/ Whether a crash from the previous run was recorded.
bool crash_note_valid(void);

/// 一行描述，形如 "panic pc=0x4201a2b4 exc=4 core=0 ..."；没有记录时返回 NULL。
/// 地址可以直接交给 addr2line 在固件 elf 上解析出函数与行号。
/// One-line description such as "panic pc=0x4201a2b4 exc=4 core=0 ..."; NULL without a record.
/// The address can go straight to addr2line against the firmware elf for a function and a line.
const char *crash_note_text(void);

/// 把记录打到串口并追加到 /sdcard/crash_log.txt，写成后清除；写不上卡就留着，下次开机再试。启动时调用一次。
/// Log the record and append it to /sdcard/crash_log.txt, clearing it once written; if the card cannot
/// be written it stays for the next boot. Call once at boot.
void crash_note_log(void);
