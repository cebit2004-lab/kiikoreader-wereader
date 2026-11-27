/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：崩溃现场。panic 时把异常类型、触发地址与描述写进 RTC 内存，下一次开机由首页显示
 * 并由串口打一遍。这台设备没有随手可用的串口，崩溃原因没有别的地方可以留。
 * 写记录只用整数赋值：panic 上下文里不能指望 printf 或分配。
 *
 * English: The panic scene. On a panic the exception type, faulting address and description go
 * into RTC memory, and the next boot shows them on the home screen and logs them once. This board
 * has no convenient console, and a crash has nowhere else to be remembered. Only integer
 * assignments happen here: the panic context cannot rely on printf or allocation.
 */

#include "crash_note.h"

#include <stdio.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"

static const char *TAG = "crash";

// RTC 内存跨软复位保留，所以崩溃后的第一次开机读到的就是上一次的现场。
// RTC memory survives a soft reset, so the first boot after a crash reads the previous scene.
typedef struct {
    uint32_t magic;
    uint32_t addr;         // 触发异常的指令地址 / instruction address that faulted
    uint32_t description;  // 描述字符串，在 flash 里，重启后仍有效 / description string, in flash
    uint32_t exception;
    uint32_t core;
    // abort()/assert 的原因文字（含调用方地址或文件行号）。它在 RAM 里，重启后就没了，所以抄一份。
    // The abort()/assert text (with the caller's address or file and line). It lives in RAM and is
    // gone after the restart, so it is copied here.
    char abort_text[96];
    // 返回地址，以及栈上最先找到的几个代码地址：没有帧指针时的简易回溯，拿 addr2line 对 ELF 查。
    // The return address and the first few code addresses found on the stack: a rough backtrace
    // without frame pointers, to be resolved against the ELF with addr2line.
    uint32_t ra;
    uint32_t stack_pc[8];
} crash_note_t;

#define CRASH_NOTE_MAGIC 0x434E4F54u  // "CNOT"
RTC_NOINIT_ATTR static crash_note_t s_note;

// 链接器由 --wrap 提供：指向被包住的那个原始实现。
// Provided by the linker through --wrap: the original implementation being wrapped.
void __real_esp_panic_handler(panic_info_t *info);

// 包住 IDF 的 panic 处理器：先记现场，再交给原来的实现去打印与重启。
// Wraps the IDF panic handler: record the scene first, then let the original print and restart.
void __wrap_esp_panic_handler(panic_info_t *info) {
    if (info != NULL) {
        s_note.addr = (uint32_t)(uintptr_t)info->addr;
        s_note.description = (uint32_t)(uintptr_t)info->description;
        s_note.exception = (uint32_t)info->exception;
        s_note.core = (uint32_t)info->core;
        s_note.abort_text[0] = '\0';
        if (g_panic_abort && g_panic_abort_details != NULL) {
            size_t i = 0;
            for (; i + 1 < sizeof(s_note.abort_text) && g_panic_abort_details[i] != '\0'; ++i) {
                s_note.abort_text[i] = g_panic_abort_details[i];
            }
            s_note.abort_text[i] = '\0';
        }
        s_note.ra = 0;
        for (size_t i = 0; i < sizeof(s_note.stack_pc) / sizeof(s_note.stack_pc[0]); ++i) s_note.stack_pc[i] = 0;
        const RvExcFrame *frame = (const RvExcFrame *)info->frame;
        if (frame != NULL) {
            s_note.ra = (uint32_t)frame->ra;
            const uint32_t *sp = (const uint32_t *)(uintptr_t)frame->sp;
            size_t found = 0;
            // 只扫 256 个字，且只在栈指针确实指向内部 RAM 或 PSRAM 时扫。
            // Scan at most 256 words, and only when the stack pointer really points at RAM.
            if (sp != NULL && ((uintptr_t)sp & 3u) == 0 && (esp_ptr_internal(sp) || esp_ptr_external_ram(sp))) {
                for (size_t w = 0; w < 256 && found < sizeof(s_note.stack_pc) / sizeof(s_note.stack_pc[0]); ++w) {
                    const void *word_addr = &sp[w];
                    if (!esp_ptr_internal(word_addr) && !esp_ptr_external_ram(word_addr)) break;
                    const uint32_t v = sp[w];
                    if (esp_ptr_executable((const void *)(uintptr_t)v)) s_note.stack_pc[found++] = v;
                }
            }
        }
        // magic 最后写：它落地才算这条记录完整。
        // The magic goes last: only once it lands is the record complete.
        s_note.magic = CRASH_NOTE_MAGIC;
    }
    __real_esp_panic_handler(info);
}

bool crash_note_valid(void) { return s_note.magic == CRASH_NOTE_MAGIC; }

const char *crash_note_text(void) {
    static char line[112];  // 首页显示用，保持原样 / shown on the home screen; kept as it was
    if (!crash_note_valid()) return NULL;
    snprintf(line, sizeof(line), "panic pc=0x%08lx exc=%lu core=%lu %s",
             (unsigned long)s_note.addr, (unsigned long)s_note.exception, (unsigned long)s_note.core,
             s_note.description != 0 ? (const char *)(uintptr_t)s_note.description : "");
    return line;
}

void crash_note_log(void) {
    const char *line = crash_note_text();
    if (line == NULL) return;
    ESP_LOGE(TAG, "%s", line);
    // 抄来的文字可能没有结尾（记录被半截覆盖），打印前强制截断。
    // The copied text may lack its terminator (a half-overwritten record); force one before printing.
    s_note.abort_text[sizeof(s_note.abort_text) - 1] = '\0';
    if (s_note.abort_text[0] != '\0') ESP_LOGE(TAG, "abort: %s", s_note.abort_text);
    ESP_LOGE(TAG, "ra=0x%08lx stack pcs: 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx",
             (unsigned long)s_note.ra, (unsigned long)s_note.stack_pc[0], (unsigned long)s_note.stack_pc[1],
             (unsigned long)s_note.stack_pc[2], (unsigned long)s_note.stack_pc[3],
             (unsigned long)s_note.stack_pc[4], (unsigned long)s_note.stack_pc[5],
             (unsigned long)s_note.stack_pc[6], (unsigned long)s_note.stack_pc[7]);
}
