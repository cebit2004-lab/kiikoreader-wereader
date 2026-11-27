/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：短音频点读，见 word_voice.h。调用方把整段 MP3 读进 PSRAM 交给本模块的任务；任务占 I2S、
 *   前补一小段静音等功放起来、逐帧解码（必要时线性重采样到 BH 时钟）、后补静音等 DMA 放空，再放手。
 *   代号 s_gen 每次 play/stop 加一，任务每帧对一次，变了就停，新的一段接着读，不重开 I2S。
 * English: Short clip playback, see word_voice.h. The caller reads the whole MP3 into PSRAM and hands it
 *   to this module's task, which claims I2S, pads silence while the amplifier wakes, decodes frame by
 *   frame (linearly resampled to the BH clock when needed), pads silence so DMA drains, then lets go.
 *   s_gen bumps on every play/stop; the task checks it each frame and stops when it changes, going
 *   straight on to the next clip without reopening I2S.
 */
#include "word_voice.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "metalio_audio.h"
#include "minimp3.h"

static const char *TAG = "word_voice";

#define AUDIO_RATE 16000
#define CLIP_MAX (256 * 1024)   // 例句也只有几十 KB / even examples are a few tens of KB
#define LEAD_MS 80              // 功放起来前的静音 / silence while the amplifier wakes
#define TAIL_MS 160             // 让 DMA 把最后一块放完 / lets DMA play out the last block
#define OUT_MAX (MINIMP3_MAX_SAMPLES_PER_FRAME * 4)
#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static uint8_t *s_next;          // 等着读的一段（归本模块）/ queued clip, owned here
static size_t s_next_len;
static atomic_uint s_gen;
static atomic_bool s_playing;
static mp3dec_t *s_dec;
static int16_t *s_pcm, *s_out;
// 重采样状态 / Resampler state.
static uint32_t s_phase, s_step;
static int16_t s_prev;

static bool cancelled(unsigned gen) { return atomic_load(&s_gen) != gen; }

static void write_silence(int ms, unsigned gen) {
    memset(s_out, 0, OUT_MAX * sizeof(int16_t));
    int left = AUDIO_RATE * ms / 1000;
    while (left > 0 && !cancelled(gen)) {
        const int n = left < OUT_MAX ? left : OUT_MAX;
        if (metalio_audio_write(METALIO_AUDIO_OWNER_VOICE, s_out, (size_t)n, pdMS_TO_TICKS(300)) < 0) return;
        left -= n;
    }
}

static int resample(const int16_t *in, int n) {
    int produced = 0;
    while (produced < OUT_MAX) {
        const uint32_t idx = s_phase >> 16;
        if ((int)idx >= n) break;
        const int32_t a = idx == 0 ? s_prev : in[idx - 1];
        const int32_t b = in[idx];
        s_out[produced++] = (int16_t)(a + (((b - a) * (int32_t)(s_phase & 0xffff)) >> 16));
        s_phase += s_step;
    }
    s_phase -= (uint32_t)n << 16;
    s_prev = in[n - 1];
    return produced;
}

// 读完整段返回 true；被打断或写不进去返回 false。/ True when the whole clip played; false when cut off or output failed.
static bool play_clip(const uint8_t *mp3, size_t len, unsigned gen) {
    mp3dec_init(s_dec);
    int out_rate = metalio_audio_actual_rate();
    if (out_rate <= 0) out_rate = metalio_audio_sample_rate();
    if (abs(out_rate - AUDIO_RATE) * 100 < AUDIO_RATE * 3) out_rate = AUDIO_RATE;
    int in_rate = 0;
    bool resampling = false;
    size_t pos = 0;
    while (pos < len) {
        if (cancelled(gen)) return false;
        mp3dec_frame_info_t info;
        const int samples = mp3dec_decode_frame(s_dec, mp3 + pos, (int)(len - pos), s_pcm, &info);
        if (info.frame_bytes <= 0) break;
        pos += (size_t)info.frame_bytes;
        if (samples <= 0) continue;
        int n = samples;
        if (info.channels == 2) {
            for (int i = 0; i < samples; ++i) s_pcm[i] = (int16_t)(((int32_t)s_pcm[2 * i] + s_pcm[2 * i + 1]) / 2);
        }
        if (info.hz != in_rate) {
            in_rate = info.hz;
            resampling = abs(out_rate - in_rate) * 100 > in_rate * 3;
            s_step = (uint32_t)(((uint64_t)in_rate << 16) / (uint64_t)out_rate);
            s_phase = 0;
            s_prev = 0;
        }
        const int16_t *out = s_pcm;
        if (resampling) { n = resample(s_pcm, samples); out = s_out; }
        if (metalio_audio_write(METALIO_AUDIO_OWNER_VOICE, out, (size_t)n, pdMS_TO_TICKS(500)) < 0) return false;
    }
    return true;
}

static uint8_t *take_next(size_t *len, unsigned *gen) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t *clip = s_next;
    *len = s_next_len;
    *gen = atomic_load(&s_gen);
    s_next = NULL;
    s_next_len = 0;
    xSemaphoreGive(s_lock);
    return clip;
}

static void voice_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        size_t len;
        unsigned gen;
        uint8_t *clip;
        bool claimed = false;
        while ((clip = take_next(&len, &gen)) != NULL) {
            if (!claimed) {
                // 别人（音乐、AI）占着就不等：不抢它们的喇叭。/ No waiting while music or the AI holds it.
                const esp_err_t err = metalio_audio_claim(METALIO_AUDIO_OWNER_VOICE, AUDIO_RATE, false, 0);
                if (err != ESP_OK) {
                    ESP_LOGI(TAG, "speaker unavailable: %s", esp_err_to_name(err));
                    heap_caps_free(clip);
                    break;
                }
                claimed = true;
                write_silence(LEAD_MS, gen);
            }
            const bool whole = play_clip(clip, len, gen);
            heap_caps_free(clip);
            if (whole) write_silence(TAIL_MS, gen);
        }
        if (claimed) metalio_audio_release(METALIO_AUDIO_OWNER_VOICE);
        // 放手前又来了一段：再走一轮。/ A clip arrived just before the release: go round again.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool more = s_next != NULL;
        if (!more) atomic_store(&s_playing, false);
        xSemaphoreGive(s_lock);
        if (more) xTaskNotifyGive(s_task);
    }
}

static bool ensure_started(void) {
    if (s_task) return true;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_dec) s_dec = heap_caps_malloc(sizeof(mp3dec_t), CAPS);
    if (!s_pcm) s_pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), CAPS);
    if (!s_out) s_out = heap_caps_malloc(OUT_MAX * sizeof(int16_t), CAPS);
    if (!s_lock || !s_dec || !s_pcm || !s_out) return false;
    // minimp3 栈上约 16 KB；任务不碰 flash，栈放 PSRAM。/ minimp3 needs ~16 KB of stack; no flash access, so PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "word_voice", 24 * 1024, NULL, 6, &s_task, 1, CAPS) != pdPASS) {
        s_task = NULL;
        return false;
    }
    return true;
}

bool word_voice_play(const char *path) {
    if (!path) return false;
    // 音乐或 AI 占着就不读，也不用读卡。/ Music or the AI holds the speaker: skip, without reading the card.
    const metalio_audio_owner_t owner = metalio_audio_owner();
    if (owner != METALIO_AUDIO_OWNER_NONE && owner != METALIO_AUDIO_OWNER_VOICE) return false;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > CLIP_MAX) return false;
    if (!ensure_started()) return false;
    uint8_t *clip = heap_caps_malloc((size_t)st.st_size, CAPS);
    if (!clip) return false;
    FILE *f = fopen(path, "rb");
    const size_t got = f ? fread(clip, 1, (size_t)st.st_size, f) : 0;
    if (f) fclose(f);
    if (got != (size_t)st.st_size) {
        heap_caps_free(clip);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_next) heap_caps_free(s_next);
    s_next = clip;
    s_next_len = got;
    atomic_fetch_add(&s_gen, 1);   // 打断正在读的 / cut off the clip in progress
    atomic_store(&s_playing, true);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

void word_voice_stop(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_next) heap_caps_free(s_next);
    s_next = NULL;
    s_next_len = 0;
    atomic_fetch_add(&s_gen, 1);
    xSemaphoreGive(s_lock);
}

bool word_voice_busy(void) { return atomic_load(&s_playing); }

void word_voice_prepare(void) { metalio_audio_prepare(); }
