/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：AI 语音对话，协议与参数取自 esp 仓库 main/realtime.c（通义千问 Omni 实时接口）。任务分工：
 *   控制任务（内部 RAM 栈，按需创建）：联网、开 I2S、建立/结束 WebSocket、会话配置、空闲超时；
 *   WebSocket 任务（PSRAM 栈，见组件里的改动）：收事件，回复音频 base64 解码后由 24 kHz 重采样到 16 kHz；
 *   录音任务（PSRAM 栈）：读麦克风、放大、base64 上传；AI 说话时不上传（板子无回声消除）；
 *   播放任务（PSRAM 栈）：从回复缓冲取音频写 I2S。
 * 内部 RAM 预算：WiFi 约 28 KB + I2S 收发约 4 KB + 控制任务栈 8 KB；其余（TLS、缓冲、其他栈）都在 PSRAM。
 *
 * English: AI voice chat; protocol and parameters follow the esp repo's main/realtime.c (Qwen Omni
 * realtime). Tasks:
 *   control (internal-RAM stack, on demand): WiFi, I2S, WebSocket open/close, session config, idle end;
 *   WebSocket (PSRAM stack, see the component patch): events; reply audio is base64-decoded and
 *     resampled from 24 kHz to 16 kHz;
 *   microphone (PSRAM stack): read, boost, base64 upload; muted while the AI speaks (no echo
 *     cancellation on this board);
 *   playback (PSRAM stack): reply buffer to I2S.
 * Internal RAM budget: WiFi ~28 KB + I2S duplex ~4 KB + the 8 KB control stack; TLS, buffers and the
 * other stacks are all PSRAM.
 */
#include "ai_voice.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "media_net.h"
#include "metalio_audio.h"
#include "music_player.h"

#if __has_include("ai_secrets.h")
#include "ai_secrets.h"
#endif
#ifndef RT_API_KEY
#define RT_API_KEY ""
#endif

static const char *TAG = "ai_voice";

#define RT_WS_BASE "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
#define RT_MODEL "qwen3.8-omni-flash-realtime"
#define RT_VOICE "Katerina"
#define AUDIO_RATE 16000
#define REPLY_RATE 24000
#define RX_CAP (64 * 1024)
#define PLAY_RING (384 * 1024)     // 16 kHz 单声道约 12 秒 / ~12 s of 16 kHz mono
#define MIC_SAMPLES 1600           // 100 ms
#define UPLOAD_GAIN 4              // BH1098G 麦克风偏小，放大后再上传 / the BH1098G mic is quiet; boost before upload
#define IDLE_END_MS 20000          // 这么久没人说话就结束 / end after this long without speech
#define PLAY_TAIL_MS 600           // 喇叭停下后麦克风再静音这么久 / keep the mic muted this long after the speaker stops
#define SEND_TIMEOUT_MS 4000
#define CTL_STACK (8 * 1024)
#define INSTRUCTIONS                                                                              \
    "你是墨水屏阅读器上的语音助手。用简短自然的中文口语回答，先说结论，一般不超过四句；"             \
    "不念标题、列表和符号。用户常在读书：可以解释词语、成语、典故、人物和历史背景，翻译外文，"       \
    "帮忙概括或讨论书里的内容，推荐相关的书。医疗、法律、财务问题只提供一般信息，并建议咨询专业人士。" \
    "没听清或意思不完整时，请对方再说一遍，不要猜。"

typedef enum { CMD_START, CMD_STOP, CMD_TOGGLE, CMD_CONFIGURE, CMD_WS_END } cmd_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmds;
static TaskHandle_t s_ctl;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR ai_status_t s_st;

static esp_websocket_client_handle_t s_ws;
static char *s_rx;                  // 一条完整事件 / one whole event
static bool s_rx_overflow;
static StreamBufferHandle_t s_play;
static _Atomic bool s_session, s_streaming, s_responding;
// 连接阶段的取消请求：控制任务可能正阻塞在等 WiFi，不能只靠命令队列。
// Cancel request during connection: the control task may be blocked waiting for WiFi, so the queue alone is not enough.
static volatile bool s_cancel;
static _Atomic int64_t s_last_activity_us, s_last_play_us;
static _Atomic int s_workers;       // 录音与播放任务数 / microphone + playback tasks alive
static unsigned s_event_id;
static uint32_t s_rs_phase;         // 24k→16k 重采样相位（Q16）/ resampler phase (Q16)
static int16_t s_rs_prev;
static int16_t *s_rs_out;

// ---------------------------------------------------------------- status

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { ++s_st.revision; xSemaphoreGive(s_lock); }
static void set_state(ai_state_t state) {
    lock();
    s_st.state = state;
    unlock();
}
static void set_error(const char *msg) {
    lock();
    snprintf(s_st.error, sizeof(s_st.error), "%s", msg ? msg : "");
    unlock();
}
static void touch(void) { atomic_store(&s_last_activity_us, esp_timer_get_time()); }
static void post(cmd_t cmd) { if (s_cmds) xQueueSend(s_cmds, &cmd, 0); }

// ---------------------------------------------------------------- sending

static void send_text(char *text, size_t len) {
    if (s_ws && esp_websocket_client_is_connected(s_ws) &&
        esp_websocket_client_send_text(s_ws, text, (int)len, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) < 0)
        ESP_LOGW(TAG, "send failed");
}

static void send_json(cJSON *root) {
    char id[16];
    snprintf(id, sizeof(id), "c%u", ++s_event_id);
    cJSON_AddStringToObject(root, "event_id", id);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text) {
        send_text(text, strlen(text));
        cJSON_free(text);
    }
}

static void send_type(const char *type) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    send_json(root);
}

static void send_session_update(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON *session = cJSON_AddObjectToObject(root, "session");
    cJSON *modalities = cJSON_AddArrayToObject(session, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddStringToObject(session, "instructions", INSTRUCTIONS);
    // 本地判断说话并手动提交（见 mic_task），关掉服务端的语音检测。
    // Speech is detected locally and committed by hand (see mic_task); server-side detection is off.
    cJSON_AddNullToObject(session, "turn_detection");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(session, "input_audio_transcription"), "model", "qwen3-asr-flash-realtime");
    cJSON *audio = cJSON_AddObjectToObject(session, "audio");
    cJSON *in_fmt = cJSON_AddObjectToObject(cJSON_AddObjectToObject(audio, "input"), "format");
    cJSON_AddStringToObject(in_fmt, "type", "pcm");
    cJSON_AddNumberToObject(in_fmt, "sample_rate", AUDIO_RATE);
    cJSON *out = cJSON_AddObjectToObject(audio, "output");
    cJSON_AddStringToObject(out, "voice", RT_VOICE);
    cJSON *out_fmt = cJSON_AddObjectToObject(out, "format");
    cJSON_AddStringToObject(out_fmt, "type", "pcm");
    cJSON_AddNumberToObject(out_fmt, "sample_rate", REPLY_RATE);
    send_json(root);
}

// ---------------------------------------------------------------- receiving

static void clear_playback(void) {
    if (!s_play) return;
    for (int i = 0; i < 20 && xStreamBufferReset(s_play) != pdPASS; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

// 24 kHz → 16 kHz 线性插值，s_rs_prev 跨块衔接。/ Linear 24 kHz → 16 kHz, s_rs_prev bridging chunks.
static int resample_reply(const int16_t *in, int n, int16_t *out, int cap) {
    const uint32_t step = (uint32_t)(((uint64_t)REPLY_RATE << 16) / AUDIO_RATE);
    int produced = 0;
    while (produced < cap) {
        const uint32_t idx = s_rs_phase >> 16;
        if ((int)idx >= n) break;
        const int32_t a = idx == 0 ? s_rs_prev : in[idx - 1];
        const int32_t b = in[idx];
        out[produced++] = (int16_t)(a + (((b - a) * (int32_t)(s_rs_phase & 0xffff)) >> 16));
        s_rs_phase += step;
    }
    s_rs_phase -= (uint32_t)n << 16;
    s_rs_prev = in[n - 1];
    return produced;
}

static void play_audio_event(char *text) {
    static const char field[] = "\"delta\":\"";
    char *start = strstr(text, field);
    char *end = start ? strchr(start + sizeof(field) - 1, '"') : NULL;
    if (!end || !s_play || !s_rs_out) return;
    start += sizeof(field) - 1;
    size_t pcm_len = 0;
    // 解码结果总在读指针之后，可原地进行。/ Decoded bytes always trail the read position, so decode in place.
    if (mbedtls_base64_decode((unsigned char *)text, RX_CAP, &pcm_len, (unsigned char *)start, (size_t)(end - start)) != 0)
        return;
    const int n = (int)(pcm_len / 2);
    if (n <= 0) return;
    const int m = resample_reply((const int16_t *)text, n, s_rs_out, RX_CAP / 2);
    xStreamBufferSend(s_play, s_rs_out, (size_t)m * sizeof(int16_t), pdMS_TO_TICKS(100));
    if (s_st.state != AI_SPEAKING) set_state(AI_SPEAKING);
}

static void handle_event(const char *text) {
    cJSON *root = cJSON_Parse(text);
    const char *type = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")) : NULL;
    if (!type) { cJSON_Delete(root); return; }
    if (!strcmp(type, "session.created")) {
        post(CMD_CONFIGURE);
    } else if (!strcmp(type, "session.updated")) {
        atomic_store(&s_streaming, true);
        touch();
        set_state(AI_LISTENING);
        ESP_LOGI(TAG, "ready");
    } else if (!strcmp(type, "input_audio_buffer.speech_started")) {
        touch();
        set_state(AI_HEARING);
    } else if (!strcmp(type, "input_audio_buffer.speech_stopped")) {
        touch();
        set_state(AI_THINKING);
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
        const char *said = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transcript"));
        if (said) {
            ESP_LOGI(TAG, "USER: %s", said);
            lock();
            snprintf(s_st.user, sizeof(s_st.user), "%s", said);
            unlock();
        }
    } else if (!strcmp(type, "response.created")) {
        touch();
        atomic_store(&s_responding, true);
        s_rs_phase = 0;
        s_rs_prev = 0;
        lock();
        s_st.reply[0] = 0;
        if (s_st.state != AI_SPEAKING) s_st.state = AI_THINKING;
        unlock();
    } else if (!strcmp(type, "response.audio_transcript.delta") || !strcmp(type, "response.text.delta")) {
        touch();
        const char *delta = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));
        if (delta) {
            lock();
            const size_t used = strlen(s_st.reply);
            snprintf(s_st.reply + used, sizeof(s_st.reply) - used, "%s", delta);
            unlock();
        }
    } else if (!strcmp(type, "response.audio_transcript.done")) {
        const char *reply = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transcript"));
        ESP_LOGI(TAG, "AI: %s", reply ? reply : "");
    } else if (!strcmp(type, "response.done")) {
        touch();
        atomic_store(&s_responding, false);
        // 没有语音的回答（没听清等）：回到等待说话，否则麦克风一直不听。
        // A reply without audio (e.g. not understood): back to listening, or the mic would stay deaf.
        if (s_st.state == AI_THINKING || s_st.state == AI_HEARING) set_state(AI_LISTENING);
        ESP_LOGI(TAG, "response done; internal=%u dma=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    } else if (!strcmp(type, "error")) {
        const cJSON *err = cJSON_GetObjectItem(root, "error");
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        ESP_LOGE(TAG, "server error: %s", msg ? msg : "");
        atomic_store(&s_responding, false);
        set_error(msg ? msg : "服务返回错误");
        if (s_st.state == AI_THINKING || s_st.state == AI_HEARING) set_state(AI_LISTENING);
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = data;
    if (!s_ws || !s_rx) return;
    switch (id) {
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code != 1 && ev->op_code != 0) break;
        if (ev->payload_offset == 0) s_rx_overflow = false;
        if (ev->payload_offset + ev->data_len < RX_CAP) memcpy(s_rx + ev->payload_offset, ev->data_ptr, ev->data_len);
        else s_rx_overflow = true;
        if (ev->payload_offset + ev->data_len >= ev->payload_len) {
            if (s_rx_overflow) { ESP_LOGW(TAG, "dropped %d-byte event", ev->payload_len); break; }
            s_rx[ev->payload_len] = 0;
            // 回复音频每秒约十几条，跳过 JSON 解析。/ Reply audio comes many times a second; skip the JSON parser.
            const size_t head = ev->payload_len < 160 ? (size_t)ev->payload_len : 160;
            const char saved = s_rx[head];
            s_rx[head] = 0;
            const bool audio = strstr(s_rx, "\"response.audio.delta\"") != NULL;
            s_rx[head] = saved;
            if (audio) { touch(); play_audio_event(s_rx); }
            else handle_event(s_rx);
        }
        break;
    case WEBSOCKET_EVENT_ERROR: {
        const int http = ev->error_handle.esp_ws_handshake_status_code;
        ESP_LOGE(TAG, "ws error type=%d http=%d", ev->error_handle.error_type, http);
        set_error(http == 401 || http == 403 ? "API Key 无效" : "连接 AI 服务失败");
        post(CMD_WS_END);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        post(CMD_WS_END);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- microphone and playback

static bool playing_recently(void) {
    return (s_play && !xStreamBufferIsEmpty(s_play)) ||
           esp_timer_get_time() - atomic_load(&s_last_play_us) < (int64_t)PLAY_TAIL_MS * 1000;
}

#define PREROLL_FRAMES 3
#define START_FRAMES 2
#define END_FRAMES 8
#define MAX_SPEECH_FRAMES 150

// TEMP 调试：把前几段上传的语音（放大后）存成 WAV，查音质。/ TEMP debug: save the first few uploaded utterances (boosted) as WAV to check quality.
#define DEBUG_WAV_MAX 5
#define DEBUG_WAV_SAMPLES (MAX_SPEECH_FRAMES * MIC_SAMPLES + PREROLL_FRAMES * MIC_SAMPLES)
static int16_t *s_dbg;
static size_t s_dbg_len;
static int s_dbg_count;

static void dbg_add(const int16_t *pcm, int n) {
    if (!s_dbg || s_dbg_count >= DEBUG_WAV_MAX) return;
    if (s_dbg_len + (size_t)n > DEBUG_WAV_SAMPLES) n = (int)(DEBUG_WAV_SAMPLES - s_dbg_len);
    memcpy(s_dbg + s_dbg_len, pcm, (size_t)n * sizeof(int16_t));
    s_dbg_len += (size_t)n;
}

static void dbg_flush(void) {
    if (!s_dbg || !s_dbg_len || s_dbg_count >= DEBUG_WAV_MAX) { s_dbg_len = 0; return; }
    mkdir("/sdcard/ai_debug", 0777);
    char path[48];
    snprintf(path, sizeof(path), "/sdcard/ai_debug/utt%d.wav", ++s_dbg_count);
    FILE *f = fopen(path, "wb");
    if (f) {
        const uint32_t data = (uint32_t)(s_dbg_len * 2), rate = AUDIO_RATE, byte_rate = AUDIO_RATE * 2;
        const uint32_t riff = 36 + data, fmt_len = 16;
        const uint16_t pcm_fmt = 1, ch = 1, align = 2, bits = 16;
        fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
        fwrite(&fmt_len, 4, 1, f); fwrite(&pcm_fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
        fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(s_dbg, 2, s_dbg_len, f);
        fclose(f);
        ESP_LOGI(TAG, "saved %s (%u samples)", path, (unsigned)s_dbg_len);
    }
    s_dbg_len = 0;
}

// 上传一段（放大后）音频。/ Upload one chunk of audio, boosted.
static void send_audio(char *msg, size_t b64_cap, int16_t *pcm, int n) {
    for (int i = 0; i < n; ++i) {
        int v = pcm[i] * UPLOAD_GAIN;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    dbg_add(pcm, n);
    const int head = snprintf(msg, 96, "{\"event_id\":\"a%u\",\"type\":\"input_audio_buffer.append\",\"audio\":\"", ++s_event_id);
    size_t b64_len = 0;
    mbedtls_base64_encode((unsigned char *)msg + head, b64_cap, &b64_len, (const unsigned char *)pcm, (size_t)n * 2);
    memcpy(msg + head + b64_len, "\"}", 3);
    send_text(msg, (size_t)head + b64_len + 2);
}

static int frame_rms(const int16_t *pcm, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) sum += (int32_t)pcm[i] * pcm[i];
    return n > 0 ? (int)sqrtf((float)(sum / n)) : 0;
}

/*
 * 本地判断说话，只上传说话的片段：一直上传 16 kHz 音频每秒约 43 KB（base64 后），这块板子的上行撑不住，
 * 实测几秒就发送超时断线。音量明显高于底噪即开始（带 0.3 秒预录），安静 0.8 秒即提交并请求回答，
 * 单句最长 15 秒。AI 说话时不判断、不上传（没有回声消除）。
 * Detect speech locally and upload only speech: streaming 16 kHz audio continuously is ~43 KB/s after
 * base64, more than this board's uplink sustains (measured: send timeout and disconnect within
 * seconds). Speech starts when the level clearly exceeds the noise floor (with 0.3 s of pre-roll);
 * 0.8 s of quiet commits it and requests a reply; one utterance is capped at 15 s. Nothing is judged
 * or uploaded while the AI speaks (no echo cancellation).
 */

static void mic_task(void *arg) {
    (void)arg;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t b64_cap = (MIC_SAMPLES * 2 / 3 + 2) * 4 + 1;
    int16_t *pcm = heap_caps_malloc(MIC_SAMPLES * sizeof(int16_t), caps);
    int16_t *preroll = heap_caps_malloc(PREROLL_FRAMES * MIC_SAMPLES * sizeof(int16_t), caps);
    char *msg = heap_caps_malloc(b64_cap + 128, caps);
    if (!s_dbg) s_dbg = heap_caps_malloc(DEBUG_WAV_SAMPLES * sizeof(int16_t), caps);
    int noise = 0, loud = 0, quiet = 0, frames = 0, pre_count = 0, pre_head = 0, level_max = 0, chunks = 0;
    bool speaking = false;
    while (pcm && preroll && msg && atomic_load(&s_session)) {
        const int n = metalio_audio_read(pcm, MIC_SAMPLES, pdMS_TO_TICKS(200));
        if (n != MIC_SAMPLES || !atomic_load(&s_streaming)) continue;
        if (s_st.state == AI_SPEAKING || s_st.state == AI_THINKING || playing_recently()) {
            speaking = false; loud = quiet = pre_count = 0;
            continue;
        }
        const int rms = frame_rms(pcm, n);
        if (rms > level_max) level_max = rms;
        if (++chunks % 10 == 0) {  // 每秒更新一次电平 / refresh the level once a second
            lock();
            s_st.mic_peak = level_max;
            unlock();
            ESP_LOGI(TAG, "mic level max=%d noise=%d", level_max, noise);
            level_max = 0;
        }
        if (!noise) noise = rms;
        const int start_th = noise * 3 > 200 ? noise * 3 : 200;
        const int end_th = noise * 2 > 150 ? noise * 2 : 150;
        if (!speaking) {
            // 底噪慢慢跟随安静时的电平。/ The noise floor slowly follows the quiet level.
            noise = (noise * 15 + rms) / 16;
            if (noise < 20) noise = 20;
            memcpy(preroll + pre_head * MIC_SAMPLES, pcm, MIC_SAMPLES * sizeof(int16_t));
            pre_head = (pre_head + 1) % PREROLL_FRAMES;
            if (pre_count < PREROLL_FRAMES) ++pre_count;
            loud = rms > start_th ? loud + 1 : 0;
            if (loud < START_FRAMES) continue;
            speaking = true;
            quiet = 0;
            frames = 0;
            touch();
            set_state(AI_HEARING);
            ESP_LOGI(TAG, "speech start rms=%d noise=%d", rms, noise);
            // 先送预录（含刚才判定为说话的几帧）。/ Send the pre-roll first, including the frames that triggered.
            for (int i = 0; i < pre_count; ++i) {
                const int idx = (pre_head + PREROLL_FRAMES - pre_count + i) % PREROLL_FRAMES;
                send_audio(msg, b64_cap, preroll + idx * MIC_SAMPLES, MIC_SAMPLES);
            }
            pre_count = 0;
            continue;
        }
        send_audio(msg, b64_cap, pcm, n);
        ++frames;
        quiet = rms < end_th ? quiet + 1 : 0;
        if (quiet >= END_FRAMES || frames >= MAX_SPEECH_FRAMES) {
            speaking = false;
            loud = 0;
            ESP_LOGI(TAG, "speech end after %d frames", frames);
            dbg_flush();
            send_type("input_audio_buffer.commit");
            send_type("response.create");
            touch();
            set_state(AI_THINKING);
        }
    }
    heap_caps_free(pcm);
    heap_caps_free(preroll);
    heap_caps_free(msg);
    atomic_fetch_sub(&s_workers, 1);
    vTaskDeleteWithCaps(NULL);
}

static void play_task(void *arg) {
    (void)arg;
    int16_t *buf = heap_caps_malloc(MIC_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    while (buf && atomic_load(&s_session)) {
        const size_t got = xStreamBufferReceive(s_play, buf, MIC_SAMPLES * sizeof(int16_t), pdMS_TO_TICKS(50));
        if (got) {
            metalio_audio_write(buf, got / sizeof(int16_t), pdMS_TO_TICKS(500));
            atomic_store(&s_last_play_us, esp_timer_get_time());
            touch();
        } else if (s_st.state == AI_SPEAKING && !atomic_load(&s_responding) && !playing_recently()) {
            metalio_audio_set_pa(false);
            set_state(AI_LISTENING);
        }
    }
    heap_caps_free(buf);
    atomic_fetch_sub(&s_workers, 1);
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------- session

static void session_end(const char *err) {
    atomic_store(&s_streaming, false);
    atomic_store(&s_session, false);
    if (s_ws) {
        esp_websocket_client_handle_t ws = s_ws;
        esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
        s_ws = NULL;  // 之后到达的事件一律忽略 / later events are ignored
        esp_websocket_client_destroy(ws);
    }
    for (int i = 0; i < 100 && atomic_load(&s_workers) > 0; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    metalio_audio_close();
    if (s_play) { vStreamBufferDeleteWithCaps(s_play); s_play = NULL; }
    heap_caps_free(s_rx);
    s_rx = NULL;
    heap_caps_free(s_rs_out);
    s_rs_out = NULL;
    atomic_store(&s_responding, false);
    lock();
    s_st.state = AI_IDLE;
    if (err) snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    unlock();
    media_net_release_if_unused();
    ESP_LOGI(TAG, "session ended %s", err ? err : "");
}

static void session_start(void) {
    if (atomic_load(&s_session)) return;
    s_cancel = false;
    lock();
    s_st.state = AI_CONNECTING;
    s_st.user[0] = s_st.reply[0] = s_st.error[0] = 0;
    s_st.mic_peak = 0;
    unlock();
    if (!RT_API_KEY[0]) { session_end("未配置 AI 服务密钥"); return; }
    // 音乐与 AI 不同时用音频和网络。/ Music and AI never share audio and network.
    if (music_active()) music_stop();
    char err[64] = {0};
    if (!media_net_acquire(err, sizeof(err), &s_cancel)) { session_end(s_cancel ? NULL : err); return; }
    // WiFi 刚连上、还没开始传数据时开 I2S（麦克风+喇叭）；分不到就先断网占 I2S 再重连。
    // Open I2S (mic + speaker) right after WiFi connects, before traffic fragments DMA RAM; failing
    // that, drop WiFi, claim I2S and reconnect.
    if (metalio_audio_open(AUDIO_RATE, true) != ESP_OK) {
        media_net_drop();
        if (metalio_audio_open(AUDIO_RATE, true) != ESP_OK) { session_end("音频设备不可用"); return; }
        if (!media_net_acquire(err, sizeof(err), &s_cancel)) { session_end(s_cancel ? NULL : err); return; }
    }
    if (s_cancel) { session_end(NULL); return; }
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_rx = heap_caps_malloc(RX_CAP + 1, caps);
    s_rs_out = heap_caps_malloc(RX_CAP, caps);
    s_play = xStreamBufferCreateWithCaps(PLAY_RING, 1, caps);
    if (!s_rx || !s_rs_out || !s_play) { session_end("内存不足"); return; }
    static char headers[256];
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", RT_API_KEY);
    const esp_websocket_client_config_t cfg = {
        .uri = RT_WS_BASE "?model=" RT_MODEL,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .task_stack = 12 * 1024,
        .buffer_size = 4096,
        .network_timeout_ms = 10000,
    };
    atomic_store(&s_session, true);
    atomic_store(&s_last_play_us, 0);
    touch();
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { session_end("内存不足"); return; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) { session_end("连接 AI 服务失败"); return; }
    atomic_store(&s_workers, 2);
    if (xTaskCreatePinnedToCoreWithCaps(mic_task, "ai_mic", 6 * 1024, NULL, 5, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "ai_play", 4 * 1024, NULL, 6, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    ESP_LOGI(TAG, "connecting; internal=%u dma=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

static void ctl_task(void *arg) {
    (void)arg;
    cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (cmd) {
            case CMD_START: session_start(); break;
            // 会话可能已结束、音乐已接手音频：只在 AI 仍在进行时收尾。/ The chat may be over with music owning audio; end only a live chat.
            case CMD_STOP: if (atomic_load(&s_session) || s_st.state != AI_IDLE) session_end(NULL); break;
            case CMD_TOGGLE:
                if (!atomic_load(&s_session)) session_start();
                else if (s_st.state == AI_SPEAKING || atomic_load(&s_responding)) {
                    // 打断：取消回答并清空播放。/ Interrupt: cancel the reply and drop queued audio.
                    send_type("response.cancel");
                    clear_playback();
                    atomic_store(&s_responding, false);
                    metalio_audio_set_pa(false);
                    touch();
                    set_state(AI_LISTENING);
                } else session_end(NULL);
                break;
            case CMD_CONFIGURE: send_session_update(); break;
            case CMD_WS_END: if (atomic_load(&s_session)) session_end(s_st.error[0] ? s_st.error : "连接已断开"); break;
            }
        }
        // 空闲超时：没人说话、也没在回答。/ Idle timeout: no speech and no reply.
        if (atomic_load(&s_session) && atomic_load(&s_streaming) && !atomic_load(&s_responding) && !playing_recently() &&
            esp_timer_get_time() - atomic_load(&s_last_activity_us) > (int64_t)IDLE_END_MS * 1000)
            session_end(NULL);
        if (!atomic_load(&s_session)) {
            taskENTER_CRITICAL(&s_mux);
            const bool done = uxQueueMessagesWaiting(s_cmds) == 0;
            if (done) s_ctl = NULL;
            taskEXIT_CRITICAL(&s_mux);
            if (done) vTaskDeleteWithCaps(NULL);
        }
    }
}

// ---------------------------------------------------------------- public API

esp_err_t ai_init(void) {
    if (s_cmds) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    return s_lock && s_cmds ? ESP_OK : ESP_ERR_NO_MEM;
}

// 控制任务会启动 WiFi（关缓存时读写 NVS），栈必须在主内部 RAM。/ The control task starts WiFi (NVS with the cache off), so its stack must be main internal RAM.
static void post_and_run(cmd_t cmd) {
    if (!s_cmds) return;
    xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(200));
    taskENTER_CRITICAL(&s_mux);
    const bool need = s_ctl == NULL;
    if (need) s_ctl = (TaskHandle_t)1;
    taskEXIT_CRITICAL(&s_mux);
    if (!need) return;
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(ctl_task, "ai_ctl", CTL_STACK, NULL, 4, &task, tskNO_AFFINITY,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "control task: no internal memory");
        set_error("内存不足");
        task = NULL;
    }
    taskENTER_CRITICAL(&s_mux);
    // 任务可能已处理完并退出（置空）；只替换占位值。/ The task may already have finished (set NULL); replace only the placeholder.
    if (s_ctl == (TaskHandle_t)1) s_ctl = task;
    taskEXIT_CRITICAL(&s_mux);
}

void ai_start(void) { post_and_run(CMD_START); }
void ai_stop(void) {
    if (!ai_active()) return;
    if (s_st.state == AI_CONNECTING) s_cancel = true;  // 立即打断等 WiFi / interrupt the WiFi wait at once
    post_and_run(CMD_STOP);
}
void ai_toggle(void) {
    if (s_st.state == AI_CONNECTING) s_cancel = true;
    post_and_run(CMD_TOGGLE);
}

void ai_get_status(ai_status_t *out) {
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

bool ai_active(void) { return atomic_load(&s_session) || s_st.state != AI_IDLE; }
