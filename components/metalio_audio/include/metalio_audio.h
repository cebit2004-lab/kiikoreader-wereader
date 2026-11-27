/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：Metalio E-INK4 Plus 的板载音频。BH1098G 蓝牙音频芯片经 UART1 AT 命令切到
 * “模式 1 本机音频”，喇叭（NS4150B 功放）和麦克风经 I2S 与主控交换 PCM；主控是
 * I2S 从机，时钟由 BH1098G 提供。
 *
 * English: On-board audio for the Metalio E-INK4 Plus. The BH1098G Bluetooth audio
 * chip is switched to "mode 1, local audio" with AT commands over UART1; the speaker
 * (NS4150B amplifier) and microphone exchange PCM with the host over I2S, where the
 * host is the I2S slave clocked by the BH1098G.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    METALIO_AUDIO_OFF,          // 未初始化 / not initialised
    METALIO_AUDIO_STARTING,     // 正在切模式 / switching the BH1098G mode
    METALIO_AUDIO_READY,        // 模式 1 已确认 / mode 1 acknowledged
    METALIO_AUDIO_FAILED,       // 芯片无应答 / no acknowledgement
} metalio_audio_state_t;

/// 启动 UART 并在后台把 BH1098G 切到模式 1；立即返回。
/// Start the UART and switch the BH1098G to mode 1 in the background; returns at once.
esp_err_t metalio_audio_init(void);
metalio_audio_state_t metalio_audio_state(void);
/// 提前在后台把 BH1098G 切到模式 1（开机后首次约 2 秒），不等待；已就绪则无动作。
/// Start switching the BH1098G to mode 1 in the background (about 2 s on first use after boot) without
/// waiting; does nothing once ready.
void metalio_audio_prepare(void);
/// 等到模式 1 就绪或失败。/ Wait until mode 1 is ready or failed.
bool metalio_audio_wait_ready(int timeout_ms);

/// I2S 的使用者。同一时刻只有一个归属者能打开、读写和关闭 I2S。
/// Users of I2S. Only one owner at a time may open, read, write and close it.
typedef enum {
    METALIO_AUDIO_OWNER_NONE,
    METALIO_AUDIO_OWNER_MUSIC,
    METALIO_AUDIO_OWNER_AI,
    METALIO_AUDIO_OWNER_SELFTEST,
} metalio_audio_owner_t;

/// 以 owner 的名义占用并打开 I2S（单声道样本）。别人占着时最多等 wait_ms 让它放手（等的是放手通知，不轮询），
/// 等不到返回 ESP_ERR_TIMEOUT；自己已占着时按需重开（已按要求开着就立即返回）。打开失败返回其他错误，并且不再占用。
/// BH1098G 时钟固定 16 kHz（不支持 AT+SAMPLE），应传 16000。I2S DMA 只能用内部 RAM，而 WiFi 开着时几乎不剩：
/// 要在启动 WiFi 之前打开。capture=false 只开喇叭（最省内存）；需要麦克风时传 true。
/// Claim I2S for owner and open it for mono samples. While someone else holds it, wait up to wait_ms for
/// it to let go (woken by the release, not polling); ESP_ERR_TIMEOUT if it does not. When owner already
/// holds it, reopen as needed (returns at once when already open as asked). Any other error means the
/// open failed and the claim is not held. The BH1098G clock is fixed at 16 kHz (no AT+SAMPLE); pass
/// 16000. I2S DMA must be internal RAM, which WiFi nearly exhausts: open it before starting WiFi.
/// capture=false opens the speaker only (least memory); pass true for the microphone.
esp_err_t metalio_audio_claim(metalio_audio_owner_t owner, int sample_rate, bool capture, int wait_ms);
/// owner 放手：关功放、关 I2S，并叫醒等着占用的一方。不是当前归属者时什么都不做。
/// owner lets go: amplifier off, I2S closed, and a waiting claimant woken. Does nothing unless owner holds it.
void metalio_audio_release(metalio_audio_owner_t owner);
/// 当前归属者。/ The current owner.
metalio_audio_owner_t metalio_audio_owner(void);
/// 等到没人占用 I2S（等放手通知），最多 wait_ms；返回是否已没人占用。
/// Wait up to wait_ms (woken by the release) until nobody holds I2S; returns whether it is free.
bool metalio_audio_wait_free(int wait_ms);
int metalio_audio_sample_rate(void);
bool metalio_audio_is_open(void);
/// 打开时实测的 BH 时钟（帧/秒）；AT+SAMPLE 不生效时与请求值不同。
/// BH clock measured at open (frames/s); differs from the request when AT+SAMPLE has no effect.
int metalio_audio_actual_rate(void);
// 发送 DMA 放空（写得不够快、补了静音）的累计次数；空闲时也会增长，按播放前后差值看。
// Running count of TX DMA underflows (writes fell behind and silence went out); it also grows while
// idle, so compare it before and after playback.
uint32_t metalio_audio_tx_underflows(void);
/// 第 index 次放空（从 0 数）的时刻，esp_timer 微秒的低 32 位；只保留最近 128 次，更早的返回 false。
/// When underflow number index (from 0) happened, as the low 32 bits of esp_timer µs; only the latest
/// 128 are kept and older ones return false.
bool metalio_audio_tx_underflow_at(uint32_t index, uint32_t *us);

/// 读写只认当前归属者：别人或已放手的一方读写返回 -1。读写可与放手/重开并发：放手先挡住新的读写，再等
/// 正在进行的那块（最多等调用方给的 wait）结束，之后读写返回 <0 或已完成的部分。
/// Read and write accept the current owner only: anyone else, or an owner that has let go, gets -1. I/O
/// may run concurrently with a release or reopen: it first turns new I/O away and waits for the chunk in
/// progress (at most the caller's wait); I/O then returns <0 or what was done so far.
/// 播放单声道 16 位样本，返回写入样本数（<0 出错）。会自动打开功放。
/// Play mono 16-bit samples; returns samples written (<0 on error). Turns the amplifier on.
int metalio_audio_write(metalio_audio_owner_t owner, const int16_t *mono, size_t samples, TickType_t wait);
/// 读取麦克风单声道 16 位样本，返回样本数。/ Read mono 16-bit mic samples; returns the count.
int metalio_audio_read(metalio_audio_owner_t owner, int16_t *mono, size_t samples, TickType_t wait);

/// 软件音量 0–100。/ Software volume 0–100.
void metalio_audio_set_volume(int percent);
int metalio_audio_volume(void);
/// 功放开关（NS4150B，扩展 IO P04）。/ Amplifier switch (NS4150B, expander P04).
void metalio_audio_set_pa(bool on);

/// 发送一条 AT 命令（自动补 CRLF），等待包含 expect 的应答行。
/// Send an AT command (CRLF appended) and wait for a reply line containing expect.
esp_err_t metalio_audio_at(const char *cmd, const char *expect, int timeout_ms,
                           char *reply, size_t reply_size);

/// 自检：播放 1 kHz 提示音并记录麦克风电平；别人占着 I2S 时不做。
/// Self-test: play a 1 kHz tone and log mic level; skipped while someone else holds I2S.
void metalio_audio_selftest(void);

#ifdef __cplusplus
}
#endif
