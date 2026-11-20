/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BH1098G 模式控制（UART1 AT）+ I2S 从机双工。协议见 CloudZao/Metalio-E-INK4-Plus
 * docs/bh1098-audio-protocol.md：AT+RX=2，等 700 ms，AT+MODE=1，应答 “SET MODE 1”。
 * I2S：BH 提供时钟，32 位立体声槽；输出把单声道样本送到左右槽，输入右移 12 位限幅。
 *
 * English: BH1098G mode control (UART1 AT) plus a full-duplex I2S slave. Protocol per
 * CloudZao/Metalio-E-INK4-Plus docs/bh1098-audio-protocol.md: AT+RX=2, wait 700 ms,
 * AT+MODE=1, reply "SET MODE 1". I2S is clocked by the BH with 32-bit stereo slots;
 * playback feeds mono samples to both slots, capture shifts right by 12 and clips.
 */
#include "metalio_audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "metalio_hw.h"

static const char *TAG = "metalio_audio";

#define BT_UART        UART_NUM_1
#define BT_TX_PIN      GPIO_NUM_39  // 主控 TX → BH / host TX to BH
#define BT_RX_PIN      GPIO_NUM_38  // 主控 RX ← BH / host RX from BH
#define I2S_BCLK_PIN   GPIO_NUM_42
#define I2S_WS_PIN     GPIO_NUM_43
#define I2S_DOUT_PIN   GPIO_NUM_44  // 主控 → 喇叭 / host to speaker
#define I2S_DIN_PIN    GPIO_NUM_45  // 麦克风 → 主控 / mic to host
#define IO_PA_EN       4            // 扩展 IO P04，NS4150B CTRL / expander P04
#define IO_SHARED_PWR  12           // 扩展 IO P14，4G/BT/功放共享电源 / shared rail

#define DEFAULT_RATE   16000
#define CHUNK_FRAMES   256          // 一次搬运的立体声帧 / stereo frames per transfer

static _Atomic metalio_audio_state_t s_state = METALIO_AUDIO_OFF;
static SemaphoreHandle_t s_at_lock;     // 一次一条 AT 事务 / one AT transaction at a time
static SemaphoreHandle_t s_i2s_lock;    // 打开/关闭与读写互斥 / open/close vs read/write
static i2s_chan_handle_t s_tx, s_rx;
static int s_rate;
static int s_actual_rate;             // 实测的 BH 时钟 / measured BH clock
static _Atomic int s_volume = 70;
static _Atomic bool s_pa_on;
static int32_t *s_txbuf, *s_rxbuf;      // CHUNK_FRAMES*2 槽 / slots each（发送按 int16 用 / TX used as int16）
static bool s_capture;                  // 当前是否开了麦克风通道 / microphone channel open
static bool s_tx32;                     // 发送用 32 位数据（与麦克风同开时）/ TX uses 32-bit data (when the mic is open)

// ---------------------------------------------------------------- UART / AT

static esp_err_t uart_up(void);
static void uart_down(void);
static void log_heap(const char *where);

// 读一行（去掉 CR/LF 与不可见字符）；超时返回 false。
// Read one line (CR/LF and non-printables stripped); false on timeout.
static bool read_line(char *line, size_t size, int64_t deadline_us) {
    size_t n = 0;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t c;
        int got = uart_read_bytes(BT_UART, &c, 1, pdMS_TO_TICKS(10));
        if (got <= 0) continue;
        if (c == '\r' || c == '\n') {
            if (n == 0) continue;
            line[n] = 0;
            return true;
        }
        if (c >= 32 && c < 127 && n + 1 < size) line[n++] = (char)c;
    }
    return false;
}

esp_err_t metalio_audio_at(const char *cmd, const char *expect, int timeout_ms,
                           char *reply, size_t reply_size) {
    if (!s_at_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_at_lock, pdMS_TO_TICKS(timeout_ms + 2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (uart_up() != ESP_OK) { xSemaphoreGive(s_at_lock); return ESP_ERR_INVALID_STATE; }
    uart_flush_input(BT_UART);
    uart_write_bytes(BT_UART, cmd, strlen(cmd));
    uart_write_bytes(BT_UART, "\r\n", 2);
    esp_err_t result = expect ? ESP_ERR_TIMEOUT : ESP_OK;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    char line[128];
    while (expect && read_line(line, sizeof(line), deadline)) {
        ESP_LOGI(TAG, "BH< %s", line);
        if (strstr(line, expect)) {
            if (reply && reply_size) snprintf(reply, reply_size, "%s", line);
            result = ESP_OK;
            break;
        }
        if (strstr(line, "ERROR") || strstr(line, "FAIL")) { result = ESP_FAIL; break; }
    }
    xSemaphoreGive(s_at_lock);
    ESP_LOGI(TAG, "BH> %s -> %s", cmd, esp_err_to_name(result));
    return result;
}

static bool apply_mode1(void) {
    (void)metalio_audio_at("AT+RX=2", NULL, 0, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(700));
    return metalio_audio_at("AT+MODE=1", "SET MODE 1", 1000, NULL, 0) == ESP_OK;
}

static void mode_task(void *arg) {
    (void)arg;
    // 共享电源开机即为高；这里只确认一次并给芯片留出启动时间。
    // The shared rail is high from boot; assert it once and give the chip time to start.
    (void)metalio_io_set(IO_SHARED_PWR, true);
    vTaskDelay(pdMS_TO_TICKS(700));
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
        ok = apply_mode1();
        if (!ok) {
            ESP_LOGW(TAG, "mode 1 attempt %d failed", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    char ver[64] = {0};
    if (ok) (void)metalio_audio_at("AT+GVER=1", "AT+GVER=", 1000, ver, sizeof(ver));
    ESP_LOGI(TAG, "BH1098G %s %s", ok ? "ready" : "unavailable", ver);
    if (!ok) uart_down();
    s_state = ok ? METALIO_AUDIO_READY : METALIO_AUDIO_FAILED;
    vTaskDeleteWithCaps(NULL);
}

// TEMP：内部 RAM 余量。/ TEMP: internal RAM headroom.
static void log_heap(const char *where) {
    ESP_LOGI(TAG, "heap %s: internal=%u dma=%u dma_largest=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

// 串口驱动只在用到音频时安装，停用后卸载：开机不常驻任何内部 RAM（U 盘模式要整块 16 KB DMA）。
// The UART driver is installed only while audio is in use and removed afterwards, so nothing stays
// resident in internal RAM from boot (USB drive mode needs one contiguous 16 KB DMA block).
static esp_err_t uart_up(void) {
    if (uart_is_driver_installed(BT_UART)) return ESP_OK;
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BT_UART, 512, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(BT_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(BT_UART, BT_TX_PIN, BT_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart: %s", esp_err_to_name(err));
        if (uart_is_driver_installed(BT_UART)) (void)uart_driver_delete(BT_UART);
    }
    return err;
}

static void uart_down(void) {
    if (!s_at_lock) return;
    xSemaphoreTake(s_at_lock, portMAX_DELAY);
    if (uart_is_driver_installed(BT_UART)) (void)uart_driver_delete(BT_UART);
    xSemaphoreGive(s_at_lock);
}

esp_err_t metalio_audio_init(void) {
    if (s_at_lock) return ESP_OK;
    s_at_lock = xSemaphoreCreateMutex();
    s_i2s_lock = xSemaphoreCreateMutex();
    // 搬运缓冲放 PSRAM，内部 RAM 留给 I2S DMA、Wi-Fi 与 USB。
    // Staging buffers live in PSRAM; internal RAM is kept for I2S DMA, Wi-Fi and USB.
    s_txbuf = heap_caps_malloc(CHUNK_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rxbuf = heap_caps_malloc(CHUNK_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_at_lock || !s_i2s_lock || !s_txbuf || !s_rxbuf) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

// 第一次用到音频时把 BH1098G 切到模式 1；失败后下次再试。
// Switch the BH1098G to mode 1 on first use; retry on the next use after a failure.
static void start_mode(void) {
    metalio_audio_state_t expected = METALIO_AUDIO_OFF;
    if (!s_at_lock || !atomic_compare_exchange_strong(&s_state, &expected, METALIO_AUDIO_STARTING)) return;
    log_heap("audio start");
    xSemaphoreTake(s_at_lock, portMAX_DELAY);
    esp_err_t err = uart_up();
    xSemaphoreGive(s_at_lock);
    if (err != ESP_OK ||
        xTaskCreatePinnedToCoreWithCaps(mode_task, "bh_mode", 4096, NULL, 4, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        uart_down();
        s_state = METALIO_AUDIO_OFF;
    }
}

metalio_audio_state_t metalio_audio_state(void) { return s_state; }

bool metalio_audio_wait_ready(int timeout_ms) {
    if (s_state == METALIO_AUDIO_OFF || s_state == METALIO_AUDIO_FAILED) {
        if (s_state == METALIO_AUDIO_FAILED) s_state = METALIO_AUDIO_OFF;
        start_mode();
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (s_state == METALIO_AUDIO_STARTING && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(20));
    return s_state == METALIO_AUDIO_READY;
}

// ---------------------------------------------------------------- I2S

// I2S 时钟由 BH 提供：写静音量出它每秒实际取走的帧数。先填满 DMA，再计时。
// The BH drives the I2S clock: write silence and time how many frames it actually takes per second.
// Fill the DMA ring first, then time the rest.
// 发送一帧 = 左右两个 int16。/ One TX frame is two int16 samples (left, right).
#define TX_FRAME_BYTES (s_tx32 ? 2 * sizeof(int32_t) : 2 * sizeof(int16_t))

static int measure_rate_locked(void) {
    memset(s_txbuf, 0, CHUNK_FRAMES * TX_FRAME_BYTES);
    size_t bytes = 0;
    for (int i = 0; i < 6; ++i)
        (void)i2s_channel_write(s_tx, s_txbuf, CHUNK_FRAMES * TX_FRAME_BYTES, &bytes, pdMS_TO_TICKS(100));
    const int64_t t0 = esp_timer_get_time();
    int frames = 0;
    while (frames < 4096) {
        if (i2s_channel_write(s_tx, s_txbuf, CHUNK_FRAMES * TX_FRAME_BYTES, &bytes, pdMS_TO_TICKS(200)) != ESP_OK)
            return 0;
        frames += (int)(bytes / TX_FRAME_BYTES);
    }
    const int64_t us = esp_timer_get_time() - t0;
    const int rate = us > 0 ? (int)((int64_t)frames * 1000000 / us) : 0;
    ESP_LOGI(TAG, "measured BH clock %d Hz (requested %d)", rate, s_rate);
    return rate;
}

static void close_locked(void) {
    if (s_tx) { (void)i2s_channel_disable(s_tx); (void)i2s_del_channel(s_tx); s_tx = NULL; }
    if (s_rx) { (void)i2s_channel_disable(s_rx); (void)i2s_del_channel(s_rx); s_rx = NULL; }
    s_rate = 0;
    s_capture = false;
    s_tx32 = false;
}

static void std_config(i2s_std_config_t *std, int sample_rate, i2s_data_bit_width_t data_bits) {
    *std = (i2s_std_config_t){
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(data_bits, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_DIN_PIN,
        },
    };
    std->clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // BH 的帧固定是 2×32 位时隙；16 位数据左对齐放进 32 位时隙，芯片看到的高 16 位不变。
    // The BH frame is always 2×32-bit slots; 16-bit data sits left-aligned in each 32-bit slot, so the
    // chip sees the same upper 16 bits.
    std->slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std->slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std->slot_cfg.ws_width = 32;
    std->slot_cfg.ws_pol = false;
    std->slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_2
    std->slot_cfg.left_align = true;
    std->slot_cfg.big_endian = false;
    std->slot_cfg.bit_order_lsb = false;
#endif
}

esp_err_t metalio_audio_open(int sample_rate, bool capture) {
    if (!metalio_audio_wait_ready(5000)) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_i2s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (s_tx && s_rate == sample_rate && (s_capture || !capture)) goto done;
    close_locked();
    log_heap("i2s before");
    // BH1098G V1.03 不支持 AT+SAMPLE（回 AT ERROR），时钟固定 16 kHz；其他采样率由调用方重采样。
    // The BH1098G V1.03 rejects AT+SAMPLE (AT ERROR); its clock stays at 16 kHz and callers resample.
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    // I2S DMA 只能在内部 RAM，WiFi 要用掉约 28 KB：发送 16 位、3×160 帧 ≈ 1.9 KB；
    // 麦克风通道只在需要时开（32 位，3×120 帧 ≈ 2.9 KB）。
    // I2S DMA must be internal RAM and WiFi takes ~28 KB: TX is 16-bit, 3×160 frames ≈ 1.9 KB; the
    // microphone channel opens only on request (32-bit, 3×120 frames ≈ 2.9 KB).
    // 每块缓冲单独分配（120 帧 × 4 字节 = 480 B），小块更容易塞进 WiFi 留下的碎片。
    // Each buffer is a separate allocation (120 frames × 4 B = 480 B); small blocks fit WiFi's leftovers.
    chan.dma_desc_num = 4;
    chan.dma_frame_num = 120;
    chan.auto_clear = true;
    err = i2s_new_channel(&chan, &s_tx, capture ? &s_rx : NULL);
    if (err != ESP_OK) goto done;
    i2s_std_config_t std;
    // 只放音时发送用 16 位省 DMA 内存；开麦克风时收发都用 32 位，与官方固件一致：
    // 收发共用 BCLK/WS，两路数据位宽不同时实测麦克风数据被打乱（服务端识别不出内容）。
    // Speaker-only TX uses 16-bit to save DMA RAM; with the mic open both directions use 32-bit as the
    // stock firmware does: TX and RX share BCLK/WS, and mismatched widths garbled the mic data (the
    // server could not recognise any words).
    s_tx32 = capture;
    if (capture) chan.dma_frame_num = 120;
    std_config(&std, sample_rate, s_tx32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT);
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err == ESP_OK && s_rx) {
        std_config(&std, sample_rate, I2S_DATA_BIT_WIDTH_32BIT);
        err = i2s_channel_init_std_mode(s_rx, &std);
    }
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err == ESP_OK && s_rx) err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) {
        s_rate = sample_rate;
        s_capture = capture;
        s_actual_rate = measure_rate_locked();
    } else close_locked();
done:
    xSemaphoreGive(s_i2s_lock);
    ESP_LOGI(TAG, "i2s open %d Hz capture=%d: %s", sample_rate, capture, esp_err_to_name(err));
    log_heap("i2s after");
    return err;
}

void metalio_audio_close(void) {
    metalio_audio_set_pa(false);
    if (!s_i2s_lock) return;
    xSemaphoreTake(s_i2s_lock, portMAX_DELAY);
    close_locked();
    xSemaphoreGive(s_i2s_lock);
    // BH 保持模式 1（共享电源不断），只释放串口驱动。/ The BH stays in mode 1 (shared rail on); only the UART driver goes.
    uart_down();
}

int metalio_audio_sample_rate(void) { return s_rate; }
bool metalio_audio_is_open(void) { return s_tx != NULL; }
int metalio_audio_actual_rate(void) { return s_actual_rate; }

void metalio_audio_set_volume(int percent) {
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}
int metalio_audio_volume(void) { return s_volume; }

void metalio_audio_set_pa(bool on) {
    if (s_pa_on == on) return;
    if (metalio_io_set(IO_PA_EN, on) == ESP_OK) s_pa_on = on;
}

int metalio_audio_write(const int16_t *mono, size_t samples, TickType_t wait) {
    if (!s_tx || !mono) return -1;
    metalio_audio_set_pa(true);
    // 平方曲线音量，Q16。/ Square-law volume in Q16.
    const int v = s_volume;
    const int32_t gain = (int32_t)((v * v * 65536LL) / 10000);
    int16_t *out16 = (int16_t *)s_txbuf;
    int32_t *out32 = s_txbuf;
    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
        for (size_t i = 0; i < n; ++i) {
            if (s_tx32) {
                const int32_t s = (int32_t)mono[done + i] * gain;  // int16 × Q16 → 32 位槽 / into a 32-bit slot
                out32[2 * i] = out32[2 * i + 1] = s;
            } else {
                const int16_t s = (int16_t)(((int32_t)mono[done + i] * gain) >> 16);
                out16[2 * i] = out16[2 * i + 1] = s;
            }
        }
        size_t bytes = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_txbuf, n * TX_FRAME_BYTES, &bytes, wait);
        done += bytes / TX_FRAME_BYTES;
        if (err != ESP_OK) break;
    }
    return (int)done;
}

int metalio_audio_read(int16_t *mono, size_t samples, TickType_t wait) {
    if (!s_rx || !mono) return -1;
    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_rxbuf, n * 2 * sizeof(int32_t), &bytes, wait);
        size_t frames = bytes / (2 * sizeof(int32_t));
        for (size_t i = 0; i < frames; ++i) {
            // 麦克风在左槽。/ Microphone on the left slot.
            int32_t v = s_rxbuf[2 * i] >> 12;
            mono[done + i] = v > INT16_MAX ? INT16_MAX : v < -INT16_MAX ? -INT16_MAX : (int16_t)v;
        }
        done += frames;
        if (err != ESP_OK || frames == 0) break;
    }
    return (int)done;
}

// ---------------------------------------------------------------- self-test

void metalio_audio_selftest(void) {
    if (metalio_audio_open(DEFAULT_RATE, true) != ESP_OK) {
        ESP_LOGE(TAG, "selftest: audio not available (state=%d)", (int)s_state);
        return;
    }
    // 1 kHz，0.6 s。/ 1 kHz for 0.6 s.
    static int16_t tone[CHUNK_FRAMES];
    const int total = DEFAULT_RATE * 6 / 10;
    int phase = 0;
    for (int sent = 0; sent < total; sent += CHUNK_FRAMES) {
        for (int i = 0; i < CHUNK_FRAMES; ++i, ++phase)
            tone[i] = (int16_t)(12000 * sinf(2.0f * (float)M_PI * 1000.0f * phase / DEFAULT_RATE));
        metalio_audio_write(tone, CHUNK_FRAMES, pdMS_TO_TICKS(200));
    }
    // 记录 1 s 麦克风两槽电平，确认麦克风在哪个槽。
    // Log 1 s of mic level for both slots to confirm which slot carries the mic.
    double sum_l = 0, sum_r = 0;
    int frames = 0, peak_l = 0, peak_r = 0;
    const int64_t end = esp_timer_get_time() + 1000000;
    while (esp_timer_get_time() < end) {
        size_t bytes = 0;
        if (i2s_channel_read(s_rx, s_rxbuf, CHUNK_FRAMES * 2 * sizeof(int32_t), &bytes, pdMS_TO_TICKS(100)) != ESP_OK)
            continue;
        for (size_t i = 0; i < bytes / (2 * sizeof(int32_t)); ++i) {
            int l = s_rxbuf[2 * i] >> 12, r = s_rxbuf[2 * i + 1] >> 12;
            sum_l += (double)l * l; sum_r += (double)r * r;
            if (abs(l) > peak_l) peak_l = abs(l);
            if (abs(r) > peak_r) peak_r = abs(r);
            ++frames;
        }
    }
    metalio_audio_close();
    log_heap("i2s closed");
    ESP_LOGI(TAG, "selftest mic frames=%d L rms=%.0f peak=%d | R rms=%.0f peak=%d", frames,
             frames ? sqrt(sum_l / frames) : 0, peak_l, frames ? sqrt(sum_r / frames) : 0, peak_r);
}
