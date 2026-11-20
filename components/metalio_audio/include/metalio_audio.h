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
/// 等到模式 1 就绪或失败。/ Wait until mode 1 is ready or failed.
bool metalio_audio_wait_ready(int timeout_ms);

/// 打开 I2S（单声道样本）。BH1098G 时钟固定 16 kHz（不支持 AT+SAMPLE），应传 16000。
/// I2S DMA 只能用内部 RAM，而 WiFi 开着时几乎不剩：要在启动 WiFi 之前打开。
/// Open I2S for mono samples. The BH1098G clock is fixed at 16 kHz (no AT+SAMPLE); pass 16000.
/// I2S DMA must be internal RAM, which WiFi nearly exhausts: open it before starting WiFi.
/// capture=false 只开喇叭（最省内存）；需要麦克风时传 true。/ capture=false opens the speaker only (least memory); pass true for the microphone.
esp_err_t metalio_audio_open(int sample_rate, bool capture);
void metalio_audio_close(void);
int metalio_audio_sample_rate(void);
bool metalio_audio_is_open(void);
/// 打开时实测的 BH 时钟（帧/秒）；AT+SAMPLE 不生效时与请求值不同。
/// BH clock measured at open (frames/s); differs from the request when AT+SAMPLE has no effect.
int metalio_audio_actual_rate(void);

/// 播放单声道 16 位样本，返回写入样本数（<0 出错）。会自动打开功放。
/// Play mono 16-bit samples; returns samples written (<0 on error). Turns the amplifier on.
int metalio_audio_write(const int16_t *mono, size_t samples, TickType_t wait);
/// 读取麦克风单声道 16 位样本，返回样本数。/ Read mono 16-bit mic samples; returns the count.
int metalio_audio_read(int16_t *mono, size_t samples, TickType_t wait);

/// 软件音量 0–100。/ Software volume 0–100.
void metalio_audio_set_volume(int percent);
int metalio_audio_volume(void);
/// 功放开关（NS4150B，扩展 IO P04）。/ Amplifier switch (NS4150B, expander P04).
void metalio_audio_set_pa(bool on);

/// 发送一条 AT 命令（自动补 CRLF），等待包含 expect 的应答行。
/// Send an AT command (CRLF appended) and wait for a reply line containing expect.
esp_err_t metalio_audio_at(const char *cmd, const char *expect, int timeout_ms,
                           char *reply, size_t reply_size);

/// 自检：播放 1 kHz 提示音并记录麦克风电平。/ Self-test: play a 1 kHz tone and log mic level.
void metalio_audio_selftest(void);

#ifdef __cplusplus
}
#endif
