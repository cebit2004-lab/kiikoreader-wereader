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
#include <stdarg.h>
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
#include "esp_wifi.h"
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
// 请求服务端直接输出 16 kHz（与 BH1098G 时钟一致），省掉重采样，下行也少三分之一。服务端若不认，
// 回复仍按 24 kHz 来：由 session.updated 里的实际采样率决定走不走重采样。
// Ask the server for 16 kHz output (the BH1098G clock), skipping resampling and cutting a third of the
// downlink. If the server ignores it the reply stays 24 kHz: the rate echoed in session.updated decides.
#define REPLY_RATE_WANTED 16000
#define REPLY_RATE_DEFAULT 24000
#define RX_CAP (64 * 1024)
#define PLAY_RING (384 * 1024)     // 16 kHz 单声道约 12 秒 / ~12 s of 16 kHz mono
#define MIC_SAMPLES 1600           // 100 ms
#define UPLOAD_GAIN 4              // BH1098G 麦克风偏小，放大后再上传 / the BH1098G mic is quiet; boost before upload
#define IDLE_END_MS 30000         // 这么久没人说话就结束 / end after this long without speech
#define PLAY_TAIL_MS 600           // 喇叭停下后麦克风再静音这么久 / keep the mic muted this long after the speaker stops
// 发送超时库会直接断开连接，WiFi 偶尔卡一下不能撞上这个时限（参考工程同为 8 秒）。
// The client drops the connection on a send timeout, so a WiFi stall must not reach it (8 s as in the reference).
#define SEND_TIMEOUT_MS 8000
#define UP_FRAMES 32               // 上传缓冲 3.2 秒 / 3.2 s of upload backlog
#define MAX_RETRIES 2              // 连接中断后自动重连次数 / automatic reconnects after a drop
#define RETRY_DELAY_MS 1500
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
static _Atomic int s_workers;       // 录音、上传与播放任务数 / microphone, upload and playback tasks alive
static _Atomic unsigned s_event_id;
// 所有发送都持有这把锁；销毁连接前先拿到它，保证没有发送还在用旧连接。
// Every send holds this lock; tearing the client down takes it first, so no send still uses the old client.
static SemaphoreHandle_t s_send_mtx;
// 录音任务只录音，上传任务只发送，中间是帧池：网络慢时不丢录音（参考工程同样分开）。
// The mic task only records and the upload task only sends, joined by a frame pool, so a slow network
// never drops recorded audio (split the same way in the reference).
typedef struct {
    int16_t pcm[MIC_SAMPLES];
    int n;        // 0 = 只是提交标记 / 0 = commit marker only
    bool commit;  // 发完后提交并请求回答 / commit and request a reply after sending
} up_frame_t;
static up_frame_t *s_frames;
static QueueHandle_t s_up_free, s_up_ready;
static int s_retries;
static int64_t s_retry_at_us;       // 0 表示没有待重连 / 0 when no reconnect is pending
static bool s_auth_failed;
static char s_last_cause[160];      // 最近一次断线原因 / cause of the latest drop
// 内存对账：会话开始前的内部 DMA 余量。/ Memory audit: internal DMA headroom before the session.
static size_t s_mem_dma_before, s_mem_dma_largest_before;
// 回复音频统计，用来确认 TLS 解密跟得上。/ Reply-audio stats, to confirm TLS decryption keeps up.
static int64_t s_reply_first_us, s_reply_last_us;
static size_t s_reply_samples;
static unsigned s_underruns;
#define PREBUFFER_MIN_MS 1000
#define PREBUFFER_MAX_MS 3000
static int s_prebuffer_ms = PREBUFFER_MIN_MS;  // 自适应起播缓冲 / adaptive start-of-playback buffer
// 刷屏握手：墨水屏刷新会让播放任务 0.1–0.3 秒拿不到 CPU，I2S 放空。在语音停顿处放行刷屏并暂停送数，
// 放空就只是把停顿拉长一点，听不出来。
// Redraw handshake: an e-paper refresh starves the playback task for 0.1–0.3 s and I2S runs dry. Granting
// the refresh at a pause in speech, with feeding paused, only lengthens that pause, which is inaudible.
#define QUIET_RMS 300              // 片段结尾低于此电平算停顿 / a chunk tail below this level counts as a pause
#define QUIET_TAIL 320             // 看结尾 20 ms / check the last 20 ms
// 界面没来领放行（菜单盖住、已离开本页）时最多等这么久，然后收回。
// Longest wait for the page to take a grant (a menu covers it, or it was left); then the grant is revoked.
#define REDRAW_TAKE_MAX_MS 800
// 界面领了放行就一直等它刷完：首次回答时字形缓存是冷的，排版加推屏可超过 0.8 秒，早先到点就恢复
// 播放，正好落在推屏中间破音。这个上限只防界面卡死。
// Once the page has taken the grant, wait until it has drawn: on the first reply the glyph cache is
// cold and layout plus push can exceed 0.8 s, and resuming at a fixed 0.8 s landed mid-push and
// crackled. This cap only guards against a stuck page.
#define REDRAW_DRAW_MAX_MS 3000
static _Atomic bool s_ui_wants, s_ui_granted, s_ui_busy;
static unsigned s_ui_pauses, s_ui_pause_ms, s_ui_pause_max_ms, s_ui_overruns;
// 24k→16k 重采样：带低通的多相 FIR（2/3 倍）。线性插值不滤高频，8–12 kHz 折回可听频段，齿音发破。
// 24k→16k resampling: a low-passed polyphase FIR (ratio 2/3). Linear interpolation does not filter, so
// 8–12 kHz folds back into the audible band and sibilants crackle.
#define RS_TAPS 48                   // 每相抽头 / taps per phase
#define RS_CUTOFF_HZ 7000
static int16_t s_rs_coef[2][RS_TAPS];  // Q15；相 0 落在样点上，相 1 在两点正中 / phase 0 on a sample, phase 1 halfway
static int16_t *s_rs_in;             // RS_TAPS 个历史样点 + 本块 / RS_TAPS history samples + this chunk
static int s_rs_pos;                 // 下一个输出在 s_rs_in 中的位置（半个输入样点为单位）/ next output position, in half input samples
static int16_t *s_rs_out;
static _Atomic bool s_ws_connected;  // 握手已完成 / handshake completed
static int s_reply_rate_req;         // 本次会话向服务端要的输出采样率 / output rate requested this session
static int s_reply_rate;             // 服务端确认的输出采样率 / output rate the server confirmed
static wifi_ps_type_t s_prev_ps;
static bool s_ps_saved;

// ---------------------------------------------------------------- session log
// 每次会话的关键事件同时打到串口并记在 PSRAM，会话结束时由控制任务追加到 /sdcard/ai_debug/ai.log，
// 不接电脑也能事后查（U 盘模式取出）。
// Each session's key events go to the serial log and a PSRAM buffer; the control task appends them to
// /sdcard/ai_debug/ai.log when the session ends, so they can be checked later without a computer
// (read in USB drive mode).

#define LOG_CAP (16 * 1024)
#define LOG_FILE "/sdcard/ai_debug/ai.log"
#define LOG_FILE_MAX (256 * 1024)
static char *s_log;
static size_t s_log_len;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void ai_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ai_log(const char *fmt, ...) {
    char line[320];
    int n = snprintf(line, sizeof(line), "[%7lld] ", (long long)(esp_timer_get_time() / 1000));
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, args);
    va_end(args);
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    ESP_LOGI(TAG, "%s", line);
    line[n++] = '\n';
    if (!s_log) return;
    taskENTER_CRITICAL(&s_log_mux);
    if (s_log_len + (size_t)n <= LOG_CAP) {
        memcpy(s_log + s_log_len, line, (size_t)n);
        s_log_len += (size_t)n;
    }
    taskEXIT_CRITICAL(&s_log_mux);
}

// 只在控制任务（内部 RAM 栈）里调用。/ Call only from the control task (internal-RAM stack).
static void log_flush(void) {
    if (!s_log || !s_log_len) return;
    mkdir("/sdcard/ai_debug", 0777);
    struct stat st;
    const bool rotate = stat(LOG_FILE, &st) == 0 && st.st_size > LOG_FILE_MAX;
    FILE *f = fopen(LOG_FILE, rotate ? "w" : "a");
    taskENTER_CRITICAL(&s_log_mux);
    const size_t len = s_log_len;
    taskEXIT_CRITICAL(&s_log_mux);
    if (f) {
        fwrite(s_log, 1, len, f);
        fclose(f);
    }
    // 写文件期间新追加的行留给下次。/ Lines appended while writing stay for the next flush.
    taskENTER_CRITICAL(&s_log_mux);
    memmove(s_log, s_log + len, s_log_len - len);
    s_log_len -= len;
    taskEXIT_CRITICAL(&s_log_mux);
}

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
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        const int64_t t0 = esp_timer_get_time();
        const int sent = esp_websocket_client_send_text(s_ws, text, (int)len, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
        const int ms = (int)((esp_timer_get_time() - t0) / 1000);
        if (sent < 0) ai_log("send failed: %u bytes after %d ms", (unsigned)len, ms);
        else if (ms > 1000) ai_log("slow send: %u bytes took %d ms", (unsigned)len, ms);
    }
    xSemaphoreGive(s_send_mtx);
}

static void send_json(cJSON *root) {
    char id[16];
    snprintf(id, sizeof(id), "c%u", atomic_fetch_add(&s_event_id, 1) + 1);
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
    cJSON_AddNumberToObject(out_fmt, "sample_rate", s_reply_rate_req);
    ai_log("session.update: reply sample_rate %d", s_reply_rate_req);
    send_json(root);
}

// ---------------------------------------------------------------- receiving

static void clear_playback(void) {
    if (!s_play) return;
    for (int i = 0; i < 20 && xStreamBufferReset(s_play) != pdPASS; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

static void resampler_init(void) {
    const float fc = (float)RS_CUTOFF_HZ / REPLY_RATE_DEFAULT;  // 相对输入采样率 / relative to the input rate
    const float half = RS_TAPS / 2;
    for (int phase = 0; phase < 2; ++phase) {
        float h[RS_TAPS], sum = 0;
        for (int j = 0; j < RS_TAPS; ++j) {
            const float d = (float)(j - (RS_TAPS / 2 - 1)) - 0.5f * (float)phase;  // 到输出点的距离 / distance to the output point
            const float x = 2.0f * fc * d;
            const float sinc = fabsf(x) < 1e-6f ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x);
            const float w = fabsf(d) >= half ? 0.0f
                            : 0.42f + 0.5f * cosf((float)M_PI * d / half) + 0.08f * cosf(2.0f * (float)M_PI * d / half);
            h[j] = sinc * w;
            sum += h[j];
        }
        for (int j = 0; j < RS_TAPS; ++j) s_rs_coef[phase][j] = (int16_t)lrintf(h[j] / sum * 32767.0f);
    }
}

static void resampler_reset(void) {
    if (s_rs_in) memset(s_rs_in, 0, RS_TAPS * sizeof(int16_t));
    s_rs_pos = RS_TAPS;  // 第一个输出以第 RS_TAPS/2 个历史样点为中心 / the first output centres on history sample RS_TAPS/2
}

// 每 3 个输入出 2 个输出；末尾 RS_TAPS 个样点留作下一块的历史。
// Two outputs per three inputs; the last RS_TAPS samples stay as history for the next chunk.
static int resample_reply(const int16_t *in, int n, int16_t *out, int cap) {
    if (!s_rs_in || n <= 0) return 0;
    memcpy(s_rs_in + RS_TAPS, in, (size_t)n * sizeof(int16_t));
    const int total = RS_TAPS + n;
    int produced = 0;
    while (produced < cap) {
        const int first = (s_rs_pos >> 1) - (RS_TAPS / 2 - 1);
        if (first + RS_TAPS > total) break;
        const int16_t *x = s_rs_in + first;
        const int16_t *h = s_rs_coef[s_rs_pos & 1];
        int64_t acc = 0;
        for (int j = 0; j < RS_TAPS; ++j) acc += (int32_t)x[j] * h[j];
        const int32_t v = (int32_t)((acc + (1 << 14)) >> 15);
        out[produced++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        s_rs_pos += 3;
    }
    memmove(s_rs_in, s_rs_in + n, RS_TAPS * sizeof(int16_t));
    s_rs_pos -= 2 * n;
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
    // 服务端已按 16 kHz 输出就直接播，否则 24k→16k 重采样。/ Play 16 kHz output as is; otherwise resample 24k→16k.
    const bool direct = s_reply_rate == AUDIO_RATE;
    const int16_t *pcm = direct ? (const int16_t *)text : s_rs_out;
    const int m = direct ? n : resample_reply((const int16_t *)text, n, s_rs_out, RX_CAP / 2);
    const int64_t now = esp_timer_get_time();
    if (!s_reply_first_us) s_reply_first_us = now;
    s_reply_last_us = now;
    s_reply_samples += (size_t)m;
    const size_t queued = xStreamBufferSend(s_play, pcm, (size_t)m * sizeof(int16_t), pdMS_TO_TICKS(100));
    if (queued < (size_t)m * sizeof(int16_t)) ai_log("playback buffer full: dropped %u bytes", (unsigned)((size_t)m * 2 - queued));
    if (s_st.state != AI_SPEAKING) {
        ai_log("reply audio started (%d samples, volume %d, i2s %d Hz measured %d)", m, metalio_audio_volume(),
               metalio_audio_sample_rate(), metalio_audio_actual_rate());
        set_state(AI_SPEAKING);
    }
}

static void handle_event(const char *text) {
    cJSON *root = cJSON_Parse(text);
    const char *type = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")) : NULL;
    if (!type) { cJSON_Delete(root); return; }
    if (!strcmp(type, "session.created")) {
        ai_log("session.created");
        post(CMD_CONFIGURE);
    } else if (!strcmp(type, "session.updated")) {
        // 服务端回显的输出格式决定回复走不走重采样；没回显就按请求的算，并把回显原样记下备查。
        // The echoed output format decides whether replies are resampled; without an echo, assume the
        // request, and log what came back for checking.
        cJSON *session = cJSON_GetObjectItem(root, "session");
        const cJSON *out = cJSON_GetObjectItem(cJSON_GetObjectItem(session, "audio"), "output");
        const cJSON *rate = cJSON_GetObjectItem(cJSON_GetObjectItem(out, "format"), "sample_rate");
        s_reply_rate = cJSON_IsNumber(rate) ? rate->valueint : s_reply_rate_req;
        cJSON_DeleteItemFromObject(session, "instructions");
        char *echo = cJSON_PrintUnformatted(session);
        ai_log("session.updated: reply %d Hz (%s); %.220s", s_reply_rate, cJSON_IsNumber(rate) ? "echoed" : "assumed",
               echo ? echo : "");
        cJSON_free(echo);
        if (s_reply_rate != AUDIO_RATE && s_reply_rate != REPLY_RATE_DEFAULT) {
            ai_log("unexpected reply rate %d; treating it as %d", s_reply_rate, REPLY_RATE_DEFAULT);
            s_reply_rate = REPLY_RATE_DEFAULT;
        }
        atomic_store(&s_streaming, true);
        touch();
        s_retries = 0;
        lock();
        s_st.state = AI_LISTENING;
        s_st.error[0] = 0;  // 重连成功，清掉“正在重连”/ reconnected: clear the reconnect notice
        unlock();
        ai_log("session.updated: ready");
    } else if (!strcmp(type, "input_audio_buffer.speech_started")) {
        touch();
        set_state(AI_HEARING);
    } else if (!strcmp(type, "input_audio_buffer.speech_stopped")) {
        touch();
        set_state(AI_THINKING);
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
        const char *said = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transcript"));
        if (said) {
            ai_log("USER: %.200s", said);
            lock();
            snprintf(s_st.user, sizeof(s_st.user), "%s", said);
            unlock();
        }
    } else if (!strcmp(type, "response.created")) {
        touch();
        atomic_store(&s_responding, true);
        resampler_reset();
        atomic_store(&s_ui_wants, false);  // 上一段遗留的刷屏请求作废 / drop a redraw request left from the last reply
        s_reply_first_us = s_reply_last_us = 0;
        s_reply_samples = 0;
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
        ai_log("AI: %.200s", reply ? reply : "");
    } else if (!strcmp(type, "response.done")) {
        touch();
        atomic_store(&s_responding, false);
        // 没有语音的回答（没听清等）：回到等待说话，否则麦克风一直不听。
        // A reply without audio (e.g. not understood): back to listening, or the mic would stay deaf.
        if (s_st.state == AI_THINKING || s_st.state == AI_HEARING) set_state(AI_LISTENING);
        // 收到的语音时长 ÷ 接收用时：大于 1 说明网络与 TLS 解密都跟得上播放。
        // Audio received ÷ time taken to receive it: above 1 means network and TLS decryption outpace playback.
        const int audio_ms = (int)(s_reply_samples * 1000 / AUDIO_RATE);
        const int recv_ms = s_reply_first_us ? (int)((s_reply_last_us - s_reply_first_us) / 1000) : 0;
        ai_log("response.done: reply audio %d ms received in %d ms (%.1fx realtime); internal=%u dma=%u", audio_ms,
               recv_ms, recv_ms > 0 ? (double)audio_ms / recv_ms : 0.0,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    } else if (!strcmp(type, "error")) {
        const cJSON *err = cJSON_GetObjectItem(root, "error");
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
        ai_log("server error: code=%s msg=%.200s", code ? code : "", msg ? msg : "");
        // 还没就绪就报错，多半是不认 16 kHz 输出：改回默认 24 kHz 再配置一次。
        // An error before the session is ready most likely rejects 16 kHz output: configure again at 24 kHz.
        if (!atomic_load(&s_streaming) && s_reply_rate_req != REPLY_RATE_DEFAULT) {
            ai_log("retrying session.update with %d Hz output", REPLY_RATE_DEFAULT);
            s_reply_rate_req = REPLY_RATE_DEFAULT;
            post(CMD_CONFIGURE);
            cJSON_Delete(root);
            return;
        }
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
    case WEBSOCKET_EVENT_CONNECTED:
        atomic_store(&s_ws_connected, true);
        ai_log("ws connected; internal=%u dma=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 8) {  // 服务端关闭帧：2 字节关闭码 + 原因 / server close frame: 2-byte code + reason
            const int code = ev->data_len >= 2 ? ((uint8_t)ev->data_ptr[0] << 8 | (uint8_t)ev->data_ptr[1]) : 0;
            ai_log("server closed: code=%d reason=%.*s", code, ev->data_len > 2 ? ev->data_len - 2 : 0,
                   ev->data_len > 2 ? ev->data_ptr + 2 : "");
            snprintf(s_last_cause, sizeof(s_last_cause), "AI 服务关闭了连接（%d）", code);
            break;
        }
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
        const esp_websocket_error_codes_t *e = &ev->error_handle;
        const int http = e->esp_ws_handshake_status_code;
        const bool connected = atomic_load(&s_ws_connected);
        ai_log("ws error (%s) type=%d http=%d tls=%s/-0x%x sock_errno=%d: %.*s",
               connected ? "after connect" : "connecting", (int)e->error_type, http,
               esp_err_to_name(e->esp_tls_last_esp_err), (unsigned)-e->esp_tls_stack_err, e->esp_transport_sock_errno,
               ev->data_len > 0 ? ev->data_len : 0, ev->data_len > 0 && ev->data_ptr ? ev->data_ptr : "");
        // 能区分原因的简短说明；重连都失败时显示在屏幕上。/ A short reason that tells causes apart; shown if reconnecting fails.
        const char *detail = ev->data_len > 0 && ev->data_ptr ? ev->data_ptr : "";
        const char *phase = strstr(detail, "write") ? "发送失败" : strstr(detail, "PONG") ? "心跳超时"
                            : strstr(detail, "read") ? "接收失败" : "网络错误";
        char *msg = s_last_cause;
        const size_t cap = sizeof(s_last_cause);
        if (http == 401 || http == 403) {
            s_auth_failed = true;  // 重连也没用 / reconnecting cannot fix this
            snprintf(msg, cap, "API Key 无效（HTTP %d）", http);
        } else if (http >= 400) {
            s_auth_failed = true;
            snprintf(msg, cap, "AI 服务拒绝连接（HTTP %d），检查模型名", http);
        } else if (connected) {
            snprintf(msg, cap, "与 AI 服务的连接中断（%s，errno %d）", phase, e->esp_transport_sock_errno);
        } else if (e->esp_tls_last_esp_err) {
            snprintf(msg, cap, "连接 AI 服务失败（%s）", esp_err_to_name(e->esp_tls_last_esp_err));
        } else {
            snprintf(msg, cap, "连接 AI 服务失败（errno %d）", e->esp_transport_sock_errno);
        }
        post(CMD_WS_END);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ai_log("ws %s", id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected");
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

// 调试：把前几段上传的语音（放大后）存成 WAV 查音质。默认关闭：SD 写入同样要借内部 DMA，而对话中
// 内部 DMA 最紧张。查音质时临时改成 5。
// Debug: save the first few uploaded utterances (boosted) as WAV to check quality. Off by default: SD
// writes also borrow internal DMA, which is scarcest during a chat. Set to 5 temporarily when needed.
#define DEBUG_WAV_MAX 0
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

// 把一段（放大后）音频交给上传任务；commit 为真时发完再提交并请求回答。缓冲满时丢弃并计数。
// Hand one chunk of audio (boosted) to the upload task; with commit, it commits and requests a reply
// after sending. Dropped and counted when the backlog is full.
static unsigned s_up_dropped;
static void queue_audio(const int16_t *pcm, int n, bool commit) {
    up_frame_t *f = NULL;
    // 提交标记必须送到，多等一会儿；音频帧不等，免得耽误录音。
    // A commit marker must get through, so wait a little; audio frames never wait, so recording never stalls.
    if (xQueueReceive(s_up_free, &f, commit ? pdMS_TO_TICKS(500) : 0) != pdTRUE) {
        if (++s_up_dropped == 1 || s_up_dropped % 10 == 0) ai_log("upload backlog full: dropped %u frames", s_up_dropped);
        return;
    }
    for (int i = 0; i < n; ++i) {
        const int v = pcm[i] * UPLOAD_GAIN;
        f->pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (n > 0) dbg_add(f->pcm, n);
    f->n = n;
    f->commit = commit;
    xQueueSend(s_up_ready, &f, 0);  // 帧池总数等于队列容量，不会满 / the pool size equals the queue depth, so never full
}

// 上传任务：只负责发送。网络慢时阻塞的是它，录音照常进行。
// Upload task: sending only. A slow network blocks this task while recording carries on.
static void upload_task(void *arg) {
    (void)arg;
    const size_t b64_cap = (MIC_SAMPLES * 2 / 3 + 2) * 4 + 1;
    char *msg = heap_caps_malloc(b64_cap + 128, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    while (atomic_load(&s_session)) {
        up_frame_t *f;
        if (xQueueReceive(s_up_ready, &f, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        // 重连期间积压的音频属于断掉的会话，直接丢弃。/ Audio queued across a reconnect belongs to the lost session; drop it.
        if (msg && atomic_load(&s_streaming)) {
            if (f->n > 0) {
                const int head = snprintf(msg, 96, "{\"event_id\":\"a%u\",\"type\":\"input_audio_buffer.append\",\"audio\":\"",
                                          atomic_fetch_add(&s_event_id, 1) + 1);
                size_t b64_len = 0;
                mbedtls_base64_encode((unsigned char *)msg + head, b64_cap, &b64_len, (const unsigned char *)f->pcm,
                                      (size_t)f->n * 2);
                memcpy(msg + head + b64_len, "\"}", 3);
                send_text(msg, (size_t)head + b64_len + 2);
            }
            if (f->commit) {
                send_type("input_audio_buffer.commit");
                send_type("response.create");
            }
        }
        xQueueSend(s_up_free, &f, 0);
    }
    heap_caps_free(msg);
    atomic_fetch_sub(&s_workers, 1);
    vTaskDeleteWithCaps(NULL);
}

static int frame_rms(const int16_t *pcm, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) sum += (int32_t)pcm[i] * pcm[i];
    return n > 0 ? (int)sqrtf((float)(sum / n)) : 0;
}

/*
 * 本地判断说话，只上传说话的片段（约 43 KB/s，base64 后）。音量明显高于底噪即开始（带 0.3 秒预录），
 * 安静 0.8 秒即提交并请求回答，单句最长 15 秒。AI 说话时不判断、不上传（没有回声消除）。
 * 注：早先“上行撑不住持续上传”的结论是在 WiFi 省电开着、硬件 AES 缺内存时测的，尚未重新验证。
 * Detect speech locally and upload only speech (~43 KB/s after base64). Speech starts when the level
 * clearly exceeds the noise floor (with 0.3 s of pre-roll); 0.8 s of quiet commits it and requests a
 * reply; one utterance is capped at 15 s. Nothing is judged or uploaded while the AI speaks (no echo
 * cancellation). Note: the earlier finding that the uplink cannot sustain continuous upload was
 * measured with WiFi power saving on and hardware AES short of memory; it has not been re-checked.
 */

static void mic_task(void *arg) {
    (void)arg;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    int16_t *pcm = heap_caps_malloc(MIC_SAMPLES * sizeof(int16_t), caps);
    int16_t *preroll = heap_caps_malloc(PREROLL_FRAMES * MIC_SAMPLES * sizeof(int16_t), caps);
    if (DEBUG_WAV_MAX && !s_dbg) s_dbg = heap_caps_malloc(DEBUG_WAV_SAMPLES * sizeof(int16_t), caps);
    int noise = 0, loud = 0, quiet = 0, frames = 0, pre_count = 0, pre_head = 0, level_max = 0, chunks = 0;
    bool speaking = false;
    while (pcm && preroll && atomic_load(&s_session)) {
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
            if (chunks % 50 == 0) ai_log("mic level max=%d noise=%d", level_max, noise);  // 每 5 秒记一次 / every 5 s
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
            ai_log("speech start rms=%d noise=%d", rms, noise);
            // 先送预录（含刚才判定为说话的几帧）。/ Send the pre-roll first, including the frames that triggered.
            for (int i = 0; i < pre_count; ++i) {
                const int idx = (pre_head + PREROLL_FRAMES - pre_count + i) % PREROLL_FRAMES;
                queue_audio(preroll + idx * MIC_SAMPLES, MIC_SAMPLES, false);
            }
            pre_count = 0;
            continue;
        }
        ++frames;
        quiet = rms < end_th ? quiet + 1 : 0;
        const bool end = quiet >= END_FRAMES || frames >= MAX_SPEECH_FRAMES;
        queue_audio(pcm, n, end);
        if (end) {
            speaking = false;
            loud = 0;
            ai_log("speech end after %d frames (upload backlog %u)", frames, (unsigned)uxQueueMessagesWaiting(s_up_ready));
            touch();
            set_state(AI_THINKING);
            dbg_flush();
        }
    }
    heap_caps_free(pcm);
    heap_caps_free(preroll);
    atomic_fetch_sub(&s_workers, 1);
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------- playback diagnostics
// 每段回答记下 I2S 放空的时刻、送数间隔过长、写入超时、刷屏放行，以及发生放空的那 100 ms 里各任务的
// CPU 用量，用来找出是谁让播放断供。播放中只往 PSRAM 记，说完后一并写日志，免得日志本身干扰播放。
// Per reply, record when I2S underflowed, long feed gaps, slow writes and redraw grants, plus every
// task's CPU use during each 100 ms in which an underflow happened, to find what starves playback.
// During playback everything goes to PSRAM; it is logged once the reply ends, so logging cannot
// disturb the sound.

#define DIAG_GAP_US 15000      // 两次写入相隔超过这么久就记 / note feed gaps longer than this
#define DIAG_SLOW_US 30000     // 写入比音频时长多用这么久就记 / note writes that overrun the audio by this
#define DIAG_TEXT 4096
#define DIAG_INCIDENTS 10      // 每段回答最多分析几次放空 / underflow intervals analysed per reply
#define DIAG_CLUSTER_MS 20     // 相隔这么近的放空算同一次 / underflows this close count as one stall
#define DIAG_CPU (configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS)

typedef struct {
    char *text;
    size_t len;
    int64_t t0;                // 本段开始出声 / sound start of this reply
    uint32_t uf_scan;          // 下一个要归类的放空序号 / next underflow index to classify
    uint32_t uf_snap;          // 上次快照时的放空计数 / underflow count at the last snapshot
    int clusters, incidents;
    uint32_t cl_start_ms, cl_last_ms, cl_count;
    unsigned uf_lost;
    bool skip_interval;        // 本区间含刷屏放行，不分析 / this interval held a redraw grant; skip it
#if DIAG_CPU
    TaskStatus_t *snap[3];     // 0/1 轮流为每次写入前的快照，2 为开始出声时 / 0/1 alternate before writes, 2 at sound start
    UBaseType_t count[3];
    UBaseType_t cap;           // 每份快照容量 / entries per snapshot
    uint32_t *used;
    int cur;
    int64_t snap_us[3];
#endif
} diag_t;
static diag_t s_diag;
static char s_diag_cl[600];    // 放空分布 / underflow clusters
static size_t s_diag_cl_len;

static void diag_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void diag_note(const char *fmt, ...) {
    if (!s_diag.text || s_diag.len + 2 >= DIAG_TEXT) return;
    int n = snprintf(s_diag.text + s_diag.len, DIAG_TEXT - s_diag.len, "  +%lld ms ",
                     (long long)((esp_timer_get_time() - s_diag.t0) / 1000));
    if (n < 0 || s_diag.len + (size_t)n >= DIAG_TEXT - 2) return;
    va_list args;
    va_start(args, fmt);
    const int m = vsnprintf(s_diag.text + s_diag.len + n, DIAG_TEXT - s_diag.len - (size_t)n, fmt, args);
    va_end(args);
    if (m < 0) return;
    size_t len = s_diag.len + (size_t)n + (size_t)m;
    if (len > DIAG_TEXT - 2) len = DIAG_TEXT - 2;
    s_diag.text[len++] = '\n';
    s_diag.text[len] = 0;
    s_diag.len = len;
}

static void diag_alloc(void) {
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.text = heap_caps_malloc(DIAG_TEXT, caps);
#if DIAG_CPU
    // 数组小于任务数时快照直接返回 0，所以按当前任务数再留余量。/ A snapshot returns nothing if the array is smaller than the task count, so leave headroom.
    s_diag.cap = uxTaskGetNumberOfTasks() + 24;
    for (int i = 0; i < 3; ++i) s_diag.snap[i] = heap_caps_malloc(s_diag.cap * sizeof(TaskStatus_t), caps);
    s_diag.used = heap_caps_malloc(s_diag.cap * sizeof(uint32_t), caps);
#endif
}

static void diag_free(void) {
    heap_caps_free(s_diag.text);
#if DIAG_CPU
    for (int i = 0; i < 3; ++i) heap_caps_free(s_diag.snap[i]);
    heap_caps_free(s_diag.used);
#endif
    memset(&s_diag, 0, sizeof(s_diag));
}

#if DIAG_CPU
static void diag_snapshot(int slot) {
    s_diag.count[slot] = s_diag.snap[slot] ? uxTaskGetSystemState(s_diag.snap[slot], s_diag.cap, NULL) : 0;
    s_diag.snap_us[slot] = esp_timer_get_time();
}

// 从快照 a 到 b 按 CPU 用量排名第 skip 起的 top 个任务，写成“名字/优先级/核 毫秒”。
// Tasks ranked skip onwards (top of them) by CPU time from snapshot a to b, as "name/priority/core ms".
static void diag_cpu_top(int a, int b, int skip, int top, char *out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    uint32_t *used = s_diag.used;
    if (!used || !s_diag.count[a] || !s_diag.count[b]) {
        snprintf(out, cap, "(no snapshot)");
        return;
    }
    for (UBaseType_t i = 0; i < s_diag.count[b]; ++i) {
        const TaskStatus_t *t = &s_diag.snap[b][i];
        used[i] = 0;
        for (UBaseType_t j = 0; j < s_diag.count[a]; ++j)
            if (s_diag.snap[a][j].xTaskNumber == t->xTaskNumber) {
                used[i] = (uint32_t)(t->ulRunTimeCounter - s_diag.snap[a][j].ulRunTimeCounter);
                break;
            }
    }
    for (int k = 0; k < skip + top; ++k) {
        int best = -1;
        for (UBaseType_t i = 0; i < s_diag.count[b]; ++i)
            if (used[i] && (best < 0 || used[i] > used[best])) best = (int)i;
        if (best < 0) break;
        const TaskStatus_t *t = &s_diag.snap[b][best];
        if (k < skip) {
            used[best] = 0;
            continue;
        }
#if configTASKLIST_INCLUDE_COREID
        const int core = t->xCoreID == tskNO_AFFINITY ? -1 : (int)t->xCoreID;
#else
        const int core = -2;
#endif
        const int n = snprintf(out + len, cap - len, "%s%.12s/p%u/c%d %u.%ums", len ? ", " : "", t->pcTaskName,
                               (unsigned)t->uxCurrentPriority, core, (unsigned)(used[best] / 1000),
                               (unsigned)(used[best] % 1000 / 100));
        if (n < 0 || (size_t)n >= cap - len) break;
        len += (size_t)n;
        used[best] = 0;
    }
}
#endif

static void diag_cluster_close(void) {
    if (!s_diag.cl_count) return;
    if (s_diag_cl_len + 24 < sizeof(s_diag_cl))
        s_diag_cl_len += (size_t)snprintf(s_diag_cl + s_diag_cl_len, sizeof(s_diag_cl) - s_diag_cl_len, " +%u×%u",
                                          (unsigned)s_diag.cl_start_ms, (unsigned)s_diag.cl_count);
    ++s_diag.clusters;
    s_diag.cl_count = 0;
}

// 把新发生的放空按时间归成一簇簇（播放中调用）。/ Group new underflows into clusters by time (during playback).
static void diag_scan_underflows(void) {
    const uint32_t now = metalio_audio_tx_underflows();
    for (uint32_t i = s_diag.uf_scan; i != now; ++i) {
        uint32_t us;
        if (!metalio_audio_tx_underflow_at(i, &us)) { ++s_diag.uf_lost; continue; }
        const uint32_t ms = (uint32_t)(us - (uint32_t)s_diag.t0) / 1000;
        if (s_diag.cl_count && ms - s_diag.cl_last_ms <= DIAG_CLUSTER_MS) {
            ++s_diag.cl_count;
        } else {
            diag_cluster_close();
            s_diag.cl_start_ms = ms;
            s_diag.cl_count = 1;
        }
        s_diag.cl_last_ms = ms;
    }
    s_diag.uf_scan = now;
}

// 跳过一段不算破音的放空（刷屏放行、断后重攒）。/ Skip underflows that are not crackles (redraw grants, rebuffering).
static void diag_rebase(void) {
    s_diag.uf_scan = s_diag.uf_snap = metalio_audio_tx_underflows();
    s_diag.skip_interval = true;
}

// 开始出声。/ Sound starts.
static void diag_begin(void) {
    s_diag.len = 0;
    if (s_diag.text) s_diag.text[0] = 0;
    s_diag_cl_len = 0;
    s_diag_cl[0] = 0;
    s_diag.t0 = esp_timer_get_time();
    s_diag.clusters = s_diag.incidents = 0;
    s_diag.cl_count = 0;
    s_diag.uf_lost = 0;
    s_diag.uf_scan = s_diag.uf_snap = metalio_audio_tx_underflows();
    s_diag.skip_interval = false;
#if DIAG_CPU
    diag_snapshot(2);
    s_diag.cur = 0;
    diag_snapshot(0);
#endif
}

// 每次写入前：区间里有放空就分析这段时间谁在占 CPU。/ Before each write: if this interval underflowed, see who held the CPU.
static void diag_before_write(void) {
    diag_scan_underflows();
#if DIAG_CPU
    const int prev = s_diag.cur, next = prev ^ 1;
    diag_snapshot(next);
    s_diag.cur = next;
    const uint32_t now = metalio_audio_tx_underflows();
    if (now != s_diag.uf_snap && !s_diag.skip_interval && s_diag.incidents < DIAG_INCIDENTS) {
        ++s_diag.incidents;
        static char top[200];  // 播放任务栈小，不放栈上 / kept off the small playback stack
        diag_cpu_top(prev, next, 0, 5, top, sizeof(top));
        diag_note("underflow x%u in last %u ms; cpu: %s", (unsigned)(now - s_diag.uf_snap),
                  (unsigned)((s_diag.snap_us[next] - s_diag.snap_us[prev]) / 1000), top);
    }
    s_diag.uf_snap = now;
#endif
    s_diag.skip_interval = false;
}

// 说完：把记下的都写进日志。/ Reply finished: log everything recorded.
static void diag_end(void) {
    diag_cluster_close();
    const unsigned dur = (unsigned)((esp_timer_get_time() - s_diag.t0) / 1000);
    ai_log("diag: %u ms of sound, %d stalls (start ms × underflows):%s%s", dur, s_diag.clusters,
           s_diag_cl_len ? s_diag_cl : " none", s_diag.uf_lost ? " (some times lost)" : "");
#if DIAG_CPU
    const int end = s_diag.cur ^ 1;
    diag_snapshot(end);
    static char top[260];  // 播放任务栈小，不放栈上 / kept off the small playback stack
    diag_cpu_top(2, end, 0, 5, top, sizeof(top));
    ai_log("diag: cpu over %u ms: %s", (unsigned)((s_diag.snap_us[end] - s_diag.snap_us[2]) / 1000), top);
    diag_cpu_top(2, end, 5, 5, top, sizeof(top));
    if (top[0]) ai_log("diag: cpu (cont.): %s", top);
#endif
    ai_log("diag: playback stack headroom %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (!s_diag.text) return;
    char *line = s_diag.text;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        ai_log("diag:%s", line);
        if (!nl) break;
        line = nl + 1;
    }
    s_diag.len = 0;
    s_diag.text[0] = 0;
}

static bool tail_quiet(const int16_t *pcm, size_t n) {
    const size_t k = n < QUIET_TAIL ? n : QUIET_TAIL;
    return frame_rms(pcm + (n - k), (int)k) < QUIET_RMS;
}

// 有刷屏请求就放行，并等它刷完。只在不出声的时刻调用。
// Grant a pending redraw and wait for it to finish. Call only while silent.
static void grant_redraw_if_wanted(void) {
    if (!atomic_exchange(&s_ui_wants, false)) return;
    const int64_t t0 = esp_timer_get_time();
    atomic_store(&s_ui_busy, true);
    atomic_store(&s_ui_granted, true);
    while (atomic_load(&s_ui_busy) && atomic_load(&s_session)) {
        const int64_t waited = esp_timer_get_time() - t0;
        if (atomic_load(&s_ui_granted)) {
            // 还没人领：到点收回。与界面的领取都用 exchange，二者只有一个成功。
            // Not taken yet: revoke when due. Both sides use exchange, so exactly one of them wins.
            if (waited >= (int64_t)REDRAW_TAKE_MAX_MS * 1000 && atomic_exchange(&s_ui_granted, false)) break;
        } else if (waited >= (int64_t)REDRAW_DRAW_MAX_MS * 1000) {
            ++s_ui_overruns;  // 界面领了却迟迟没刷完 / taken but never finished
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    atomic_store(&s_ui_granted, false);
    atomic_store(&s_ui_busy, false);
    const unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    if (s_diag.t0) diag_note("redraw pause %u ms", ms);
    ++s_ui_pauses;
    s_ui_pause_ms += ms;
    if (ms > s_ui_pause_max_ms) s_ui_pause_max_ms = ms;
}

static void play_task(void *arg) {
    (void)arg;
    int16_t *buf = heap_caps_malloc(MIC_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t played = 0, short_writes = 0;
    bool primed = false;  // 已攒够、正在播 / buffered enough and playing
    // I2S 放空次数只算正在播的区间（每次停下时末尾会多记两三次）。
    // I2S underflows count only while playing (each stop adds two or three at the tail).
    uint32_t underflow_mark = 0, underflows = 0;
    int64_t feed_end_us = 0;  // 上次写完的时刻 / when the last write finished
    while (buf && atomic_load(&s_session)) {
        if (!primed) {
            // 服务端边生成边发，速度约等于实时（实测 0.9 倍）：先攒够再播，断了就重新攒，
            // 宁可停顿一次，也不要一卡一卡。回答已发完时有多少播多少。
            // The server streams as it generates, at about realtime (0.9x measured): buffer before
            // playing and again after running dry, so a gap is one pause rather than constant
            // stutter. Once the reply is complete, play whatever is left.
            const size_t have = xStreamBufferBytesAvailable(s_play);
            const size_t need = (size_t)s_prebuffer_ms * AUDIO_RATE / 1000 * sizeof(int16_t);
            if (have > 0 && (have >= need || !atomic_load(&s_responding))) {
                primed = true;
                underflow_mark = metalio_audio_tx_underflows();
                if (!played) diag_begin();
                else {
                    diag_note("resume after rebuffer (%u bytes buffered)", (unsigned)have);
                    diag_rebase();
                }
                feed_end_us = 0;
            } else {
                // 回答说完（或被打断后已播过）：收尾并写诊断。/ Reply finished (or played, then interrupted): wrap up and log diagnostics.
                if (!have && !atomic_load(&s_responding) && !playing_recently() && (s_st.state == AI_SPEAKING || played)) {
                    if (played) diag_end();
                    s_diag.t0 = 0;
                    metalio_audio_set_pa(false);
                    ai_log("reply played: %u ms, underruns %u, prebuffer %d ms, i2s underflows %u, "
                           "redraw pauses %u (%u ms, longest %u ms, overruns %u), short writes %u",
                           (unsigned)(played * 1000 / AUDIO_RATE), s_underruns, s_prebuffer_ms, (unsigned)underflows,
                           s_ui_pauses, s_ui_pause_ms, s_ui_pause_max_ms, s_ui_overruns, (unsigned)short_writes);
                    underflows = 0;
                    s_ui_pauses = s_ui_pause_ms = s_ui_pause_max_ms = s_ui_overruns = 0;
                    // 这次断过，下次多攒一点。/ It ran dry this time: buffer more next time.
                    if (s_underruns && s_prebuffer_ms < PREBUFFER_MAX_MS) s_prebuffer_ms += 500;
                    played = short_writes = 0;
                    s_underruns = 0;
                    atomic_store(&s_ui_wants, false);  // 说完了，界面按状态变化自己重绘 / done: the page redraws on the state change
                    if (s_st.state == AI_SPEAKING) set_state(AI_LISTENING);
                }
                // 还没出声（起播前缓冲或断后重攒）：随时可以刷屏。/ Not sounding yet (pre-buffering or rebuffering): redraws are free.
                grant_redraw_if_wanted();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }
        const size_t got = xStreamBufferReceive(s_play, buf, MIC_SAMPLES * sizeof(int16_t), pdMS_TO_TICKS(20));
        if (got) {
            diag_before_write();
            const int64_t w0 = esp_timer_get_time();
            if (feed_end_us && w0 - feed_end_us > DIAG_GAP_US)
                diag_note("feed gap %u ms (%u bytes waiting)", (unsigned)((w0 - feed_end_us) / 1000),
                          (unsigned)xStreamBufferBytesAvailable(s_play));
            const int wrote = metalio_audio_write(buf, got / sizeof(int16_t), pdMS_TO_TICKS(500));
            feed_end_us = esp_timer_get_time();
            const int64_t audio_us = (int64_t)(got / sizeof(int16_t)) * 1000000 / AUDIO_RATE;
            if (feed_end_us - w0 > audio_us + DIAG_SLOW_US)
                diag_note("slow write %u ms for %u ms of audio", (unsigned)((feed_end_us - w0) / 1000),
                          (unsigned)(audio_us / 1000));
            played += wrote > 0 ? (size_t)wrote : 0;
            if (wrote < (int)(got / sizeof(int16_t))) ++short_writes;
            atomic_store(&s_last_play_us, esp_timer_get_time());
            touch();
            // 这段以静音结尾 = 语音停顿：在这里放行刷屏，放空的只是停顿。停顿期间的放空不算破音。
            // This chunk ends in silence, i.e. a pause in speech: grant a redraw here, so only the pause runs
            // dry. Underflows during that pause are not crackles, so they are left out of the count.
            if (atomic_load(&s_ui_wants) && tail_quiet(buf, got / sizeof(int16_t))) {
                diag_scan_underflows();
                const uint32_t before = metalio_audio_tx_underflows();
                grant_redraw_if_wanted();
                underflow_mark += metalio_audio_tx_underflows() - before;
                diag_rebase();
                feed_end_us = 0;
            }
        } else {
            // 回答还在来、缓冲却空了：记一次断续并重新攒。/ Reply still coming but the buffer ran dry: count it and rebuffer.
            diag_scan_underflows();
            if (played && atomic_load(&s_responding)) {
                ++s_underruns;
                diag_note("buffer ran dry: rebuffering");
            }
            underflows += metalio_audio_tx_underflows() - underflow_mark;
            primed = false;
        }
    }
    heap_caps_free(buf);
    atomic_fetch_sub(&s_workers, 1);
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------- session

static void log_mem(const char *where) {
    ai_log("mem %s: internal=%u dma=%u dma_largest=%u", where, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

static bool ws_open(void) {
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
    s_last_cause[0] = 0;
    atomic_store(&s_ws_connected, false);
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) { snprintf(s_last_cause, sizeof(s_last_cause), "内存不足"); return false; }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    s_ws = ws;
    xSemaphoreGive(s_send_mtx);
    if (esp_websocket_client_start(ws) != ESP_OK) {
        snprintf(s_last_cause, sizeof(s_last_cause), "连接 AI 服务失败");
        return false;
    }
    log_mem("connecting");
    return true;
}

// 先拿发送锁再摘下连接：之后不会再有发送用到它，才可以销毁。
// Take the send lock before detaching the client: once nothing can send on it, it is safe to destroy.
static void ws_close(void) {
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;  // 之后到达的事件一律忽略 / later events are ignored
    xSemaphoreGive(s_send_mtx);
    if (ws) {
        esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(ws);
    }
    atomic_store(&s_ws_connected, false);
}

static void session_end(const char *err) {
    atomic_store(&s_streaming, false);
    atomic_store(&s_session, false);
    s_retry_at_us = 0;
    ai_log("session end: %s", err ? err : "(normal)");
    ws_close();
    // 录音任务最多阻塞 200 ms，上传任务的发送已被 ws_close 等完。
    // The mic task blocks at most 200 ms; ws_close already waited out any send in progress.
    for (int i = 0; i < 150 && atomic_load(&s_workers) > 0; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    const bool workers_done = atomic_load(&s_workers) == 0;
    if (!workers_done) ai_log("workers still running after 3 s: keeping their buffers");
    metalio_audio_close();
    // 任务没退出就不释放它们还在用的缓冲：宁可漏一次内存，也不能写坏堆。
    // Buffers still in use by a live task are kept: leaking once beats corrupting the heap.
    if (workers_done) {
        if (s_play) { vStreamBufferDeleteWithCaps(s_play); s_play = NULL; }
        if (s_up_free) { vQueueDeleteWithCaps(s_up_free); s_up_free = NULL; }
        if (s_up_ready) { vQueueDeleteWithCaps(s_up_ready); s_up_ready = NULL; }
        heap_caps_free(s_frames);
        s_frames = NULL;
        heap_caps_free(s_rx);
        s_rx = NULL;
        heap_caps_free(s_rs_out);
        s_rs_out = NULL;
        heap_caps_free(s_rs_in);
        s_rs_in = NULL;
        diag_free();
    }
    atomic_store(&s_responding, false);
    atomic_store(&s_ui_wants, false);
    atomic_store(&s_ui_granted, false);
    atomic_store(&s_ui_busy, false);
    if (s_ps_saved) {
        (void)esp_wifi_set_ps(s_prev_ps);
        s_ps_saved = false;
    }
    lock();
    s_st.state = AI_IDLE;
    if (err && err != s_st.error) snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    unlock();
    media_net_release_if_unused();
    // 内存对账：结束后应回到开始前的水平。/ Memory audit: after the end, headroom should be back where it started.
    const size_t dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ai_log("mem audit: dma %u -> %u (%+d), dma_largest %u -> %u (%+d)%s", (unsigned)s_mem_dma_before, (unsigned)dma,
           (int)dma - (int)s_mem_dma_before, (unsigned)s_mem_dma_largest_before, (unsigned)largest,
           (int)largest - (int)s_mem_dma_largest_before,
           (int)dma + 512 < (int)s_mem_dma_before ? "  <-- NOT RETURNED" : "");
    log_flush();
}

// 连接断了：还能重连就稍后重连（麦克风、喇叭、WiFi 都保留），否则结束会话。
// The connection dropped: reconnect shortly if retries remain (mic, speaker and WiFi stay up), else end.
static void ws_lost(void) {
    atomic_store(&s_streaming, false);
    ws_close();
    clear_playback();
    atomic_store(&s_responding, false);
    const char *cause = s_last_cause[0] ? s_last_cause : "连接已断开";
    if (!s_auth_failed && s_retries < MAX_RETRIES) {
        ++s_retries;
        s_retry_at_us = esp_timer_get_time() + (int64_t)RETRY_DELAY_MS * 1000;
        ai_log("connection lost (%s); reconnect %d/%d in %d ms", cause, s_retries, MAX_RETRIES, RETRY_DELAY_MS);
        char msg[sizeof(s_st.error)];
        snprintf(msg, sizeof(msg), "连接中断，正在重连（%d/%d）…", s_retries, MAX_RETRIES);
        lock();
        s_st.state = AI_CONNECTING;
        snprintf(s_st.error, sizeof(s_st.error), "%s", msg);
        unlock();
        return;
    }
    char final_err[sizeof(s_last_cause)];
    snprintf(final_err, sizeof(final_err), "%s", cause);
    session_end(final_err);
}

static void session_start(void) {
    if (atomic_load(&s_session)) return;
    s_cancel = false;
    s_retries = 0;
    s_retry_at_us = 0;
    s_auth_failed = false;
    s_up_dropped = 0;
    s_reply_rate_req = REPLY_RATE_WANTED;
    s_reply_rate = REPLY_RATE_DEFAULT;
    lock();
    s_st.state = AI_CONNECTING;
    s_st.user[0] = s_st.reply[0] = s_st.error[0] = 0;
    s_st.mic_peak = 0;
    unlock();
    s_mem_dma_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_mem_dma_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ai_log("=== session start; model %s", RT_MODEL);
    log_mem("before");
    if (!RT_API_KEY[0]) { session_end("未配置 AI 服务密钥"); return; }
    // 音乐与 AI 不同时用音频和网络。/ Music and AI never share audio and network.
    if (music_active()) music_stop();
    char err[64] = {0};
    const int64_t t_start = esp_timer_get_time();
    // 先开 I2S（麦克风+喇叭）再连 WiFi：I2S DMA 只能用内部 RAM，实测 WiFi 连上后最大空块只剩约 5.8 KB，
    // 先连网时 I2S 一次也没分到过，每次都得断网、开 I2S、再连一遍，白等约 5 秒。WiFi 已被别的功能连着
    // 而分不到时，才断网再开。
    // Open I2S (mic + speaker) before WiFi: I2S DMA must be internal RAM, and once WiFi is up the largest
    // free block measured only ~5.8 KB. Connecting first never once left room for I2S, so every chat
    // dropped WiFi, opened I2S and connected again, wasting ~5 s. Only when WiFi is already up for
    // something else and I2S does not fit is WiFi dropped first.
    if (metalio_audio_open(AUDIO_RATE, true) != ESP_OK) {
        ai_log("i2s did not fit; dropping wifi first");
        media_net_drop();
        if (metalio_audio_open(AUDIO_RATE, true) != ESP_OK) { session_end("音频设备不可用"); return; }
    }
    const int64_t t_i2s = esp_timer_get_time();
    if (s_cancel) { session_end(NULL); return; }
    if (!media_net_acquire(err, sizeof(err), &s_cancel)) { session_end(s_cancel ? NULL : err); return; }
    if (s_cancel) { session_end(NULL); return; }
    ai_log("wifi + i2s ready: i2s %d Hz measured %d; audio %d ms, wifi %d ms", metalio_audio_sample_rate(),
           metalio_audio_actual_rate(), (int)((t_i2s - t_start) / 1000), (int)((esp_timer_get_time() - t_i2s) / 1000));
    log_mem("wifi + i2s");
    // 实时语音要求上下行都及时：对话期间关闭 WiFi 省电（参考工程与在线升级同样处理），结束时恢复。
    // Realtime voice needs prompt uplink and downlink: WiFi power saving is off for the chat (as in the
    // reference project and online update), restored when it ends.
    if (!s_ps_saved && esp_wifi_get_ps(&s_prev_ps) == ESP_OK && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) s_ps_saved = true;
    ai_log("wifi power save off: %s (was %d)", s_ps_saved ? "ok" : "failed", (int)s_prev_ps);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_rx = heap_caps_malloc(RX_CAP + 1, caps);
    s_rs_out = heap_caps_malloc(RX_CAP, caps);
    s_rs_in = heap_caps_malloc((RS_TAPS + RX_CAP / 2) * sizeof(int16_t), caps);
    resampler_reset();
    s_play = xStreamBufferCreateWithCaps(PLAY_RING, 1, caps);
    s_frames = heap_caps_malloc(UP_FRAMES * sizeof(up_frame_t), caps);
    s_up_free = xQueueCreateWithCaps(UP_FRAMES, sizeof(up_frame_t *), caps);
    s_up_ready = xQueueCreateWithCaps(UP_FRAMES, sizeof(up_frame_t *), caps);
    diag_alloc();  // 失败只是没有诊断 / failure only loses the diagnostics
    if (!s_rx || !s_rs_out || !s_rs_in || !s_play || !s_frames || !s_up_free || !s_up_ready) { session_end("内存不足"); return; }
    for (int i = 0; i < UP_FRAMES; ++i) {
        up_frame_t *f = &s_frames[i];
        xQueueSend(s_up_free, &f, 0);
    }
    atomic_store(&s_session, true);
    atomic_store(&s_last_play_us, 0);
    touch();
    // 先起工作任务再连接：首次连接失败转入重连时它们也已就位（未就绪前它们什么都不做）。
    // Workers start before connecting, so they are in place even when the first attempt fails into a
    // reconnect (they idle until the session is ready).
    atomic_store(&s_workers, 3);
    if (xTaskCreatePinnedToCoreWithCaps(mic_task, "ai_mic", 6 * 1024, NULL, 6, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    if (xTaskCreatePinnedToCoreWithCaps(upload_task, "ai_up", 6 * 1024, NULL, 5, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    // 播放任务要在说完时格式化诊断日志（printf 栈深），给足 10 KB（PSRAM）。
    // The playback task formats diagnostics when a reply ends (printf is stack-hungry): 10 KB, in PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "ai_play", 10 * 1024, NULL, 6, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    if (atomic_load(&s_workers) != 3) { session_end("内存不足"); return; }
    if (!ws_open()) ws_lost();
}

static void ctl_task(void *arg) {
    (void)arg;
    cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
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
            // 一次断线会先后收到 ERROR 与 DISCONNECTED：已摘下连接就忽略后面的。
            // One drop brings both ERROR and DISCONNECTED: once the client is detached, ignore the rest.
            case CMD_WS_END: if (atomic_load(&s_session) && s_ws) ws_lost(); break;
            }
        }
        // 到点重连。/ Reconnect when due.
        if (atomic_load(&s_session) && !s_ws && s_retry_at_us && esp_timer_get_time() >= s_retry_at_us) {
            s_retry_at_us = 0;
            ai_log("reconnecting (%d/%d)", s_retries, MAX_RETRIES);
            if (!ws_open()) ws_lost();
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
    s_send_mtx = xSemaphoreCreateMutex();
    resampler_init();
    s_log = heap_caps_malloc(LOG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  // 失败只是不记日志 / failure only disables the log
    return s_lock && s_cmds && s_send_mtx ? ESP_OK : ESP_ERR_NO_MEM;
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

bool ai_redraw_slot(void) {
    // 没有播放任务就没有要保护的声音。/ Without the playback task there is no sound to protect.
    if (!atomic_load(&s_session)) return true;
    if (atomic_exchange(&s_ui_granted, false)) return true;  // 播放正停在停顿处等着 / playback is holding at a pause
    atomic_store(&s_ui_wants, true);
    return false;
}

void ai_redraw_done(void) { atomic_store(&s_ui_busy, false); }

bool ai_audio_busy(void) {
    return atomic_load(&s_session) &&
           (atomic_load(&s_responding) || s_st.state == AI_SPEAKING || playing_recently());
}

bool ai_active(void) { return atomic_load(&s_session) || s_st.state != AI_IDLE; }
