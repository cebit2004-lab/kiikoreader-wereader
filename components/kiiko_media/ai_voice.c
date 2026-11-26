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

#include <errno.h>
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
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/stats.h"
#include "mbedtls/base64.h"
#include "media_net.h"
#include "metalio_audio.h"
#include "music_player.h"
#include "psram_stream.h"

#if __has_include("ai_secrets.h")
#include "ai_secrets.h"
#endif
#ifndef RT_API_KEY
#define RT_API_KEY ""
#endif

static const char *TAG = "ai_voice";

#define RT_HOST "dashscope.aliyuncs.com"
#define RT_WS_BASE "wss://" RT_HOST "/api-ws/v1/realtime"
#define RT_MODEL "qwen-audio-3.1-realtime-flash"
// qwen-audio-3.1-realtime-flash 只认 long* 系列音色（Katerina 会报 invalid_value 并断开）；
// longanqian 是百炼官方示例用的音色。
// qwen-audio-3.1-realtime-flash only accepts the long* voices (Katerina gets invalid_value and a
// disconnect); longanqian is the voice in Bailian's official example.
#define RT_VOICE "longanqian"
#define AUDIO_RATE 16000
// 请求服务端直接输出 16 kHz（与 BH1098G 时钟一致），省掉重采样，下行也少三分之一。服务端若不认，
// 回复仍按 24 kHz 来：由 session.updated 里的实际采样率决定走不走重采样。
// Ask the server for 16 kHz output (the BH1098G clock), skipping resampling and cutting a third of the
// downlink. If the server ignores it the reply stays 24 kHz: the rate echoed in session.updated decides.
#define REPLY_RATE_WANTED 16000
#define REPLY_RATE_DEFAULT 24000
#define RX_CAP (64 * 1024)
// 回复缓冲：服务端约以 1.5 倍实时速度发完整段回答，长回答在播放端越积越多（原 384 KB 约 12 秒，
// 一千字的回答就溢出丢音）。2 MB 约 65 秒，回答长到约 3 分钟才会满；满了就让接收等播放腾地方（见
// play_audio_event），不再丢。分不到就退一档。
// Reply buffer: the server sends a whole reply at about 1.5x realtime, so a long reply piles up at the
// player (the old 384 KB, ~12 s, overflowed and dropped audio on a 1000-character answer). 2 MB is
// ~65 s and fills only for replies of about 3 minutes; when full, receiving waits for playback to make
// room (see play_audio_event) instead of dropping. Falls back a size if it cannot be allocated.
static const size_t k_play_ring_sizes[] = {2 * 1024 * 1024, 1024 * 1024, 384 * 1024};
#define MIC_SAMPLES 1600           // 100 ms
#define UPLOAD_GAIN 4              // BH1098G 麦克风偏小，放大后再上传 / the BH1098G mic is quiet; boost before upload
#define IDLE_END_MS 30000         // 这么久没人说话就结束 / end after this long without speech
#define PLAY_TAIL_MS 600           // 喇叭停下后麦克风再静音这么久 / keep the mic muted this long after the speaker stops
// 发送超时库会直接断开连接，WiFi 偶尔卡一下不能撞上这个时限（参考工程同为 8 秒）。
// The client drops the connection on a send timeout, so a WiFi stall must not reach it (8 s as in the reference).
#define SEND_TIMEOUT_MS 8000
// 打断时的取消通知只是尽力而为：本地已经停下，发不出去也不等（接收任务收一帧时占着客户端锁，
// 实测一等就是 8 秒，期间界面毫无反应）。
// The cancel notice on interrupt is best effort: playback has already stopped locally, so it never
// waits long (the receive task holds the client lock while reading a frame, and waiting on it once
// took the full 8 s with the page frozen).
#define CANCEL_TIMEOUT_MS 1000
// 等回答（已提交、回答进行中）却这么久收不到任何数据：下行卡死，按断线处理并自动重连。服务端约
// 1.5 倍实时发送，正常不会断流这么久。
// Waiting for a reply (committed, or a reply in progress) with no data at all for this long: the
// downlink is stuck, so treat it as a drop and reconnect. The server sends at ~1.5x realtime and never
// pauses this long normally.
#define RX_STALL_MS 10000
// 连接（TCP+TLS+握手）与收发的网络超时。部分网络刚连上 WiFi 时前几个 SYN 会丢（实测 10 秒超时首连
// 必败、重试 6 秒连上；lwIP 重传在 0、1.5、4.5、10.5 秒），给到 20 秒，首连一次连上，不再报“重连”。
// 收发卡住由 RX_STALL_MS 与强制关套接字兜底，这个值不再决定反应速度。
// Network timeout for connecting (TCP+TLS+handshake) and I/O. On some networks the first SYNs after
// joining WiFi are lost (a 10 s timeout failed every first connect, the retry made it in 6 s; lwIP
// retransmits at 0, 1.5, 4.5 and 10.5 s), so allow 20 s and connect at the first attempt without a
// "reconnecting" notice. Stuck I/O is covered by RX_STALL_MS and the forced socket close instead.
#define NET_TIMEOUT_MS 20000
// 关连接时等 WebSocket 任务退出的上限；超时就让它自己收尾，控制任务不陪着卡住。
// Longest wait for the WebSocket task to exit on close; past it the task finishes on its own and the
// control task does not hang with it.
#define REAP_WAIT_MS 2000
#define UP_FRAMES 32               // 上传缓冲 3.2 秒 / 3.2 s of upload backlog
#define MAX_RETRIES 2              // 连接中断后自动重连次数 / automatic reconnects after a drop
#define RETRY_DELAY_MS 1500
#define CTL_STACK (8 * 1024)
// 卡死诊断：控制任务一步超过 CTL_SLOW_MS 记一笔，超过 CTL_STUCK_MS 由播放任务报出卡在哪一步。
// Hang diagnostics: a control step over CTL_SLOW_MS is logged; past CTL_STUCK_MS the playback task
// reports which step it is stuck in.
#define CTL_SLOW_MS 1500
#define CTL_STUCK_MS 5000
#define INSTRUCTIONS                                                                              \
    "你是墨水屏阅读器上的语音助手。用简短自然的中文口语回答，先说结论，一般不超过四句；"             \
    "不念标题、列表和符号。用户常在读书：可以解释词语、成语、典故、人物和历史背景，翻译外文，"       \
    "帮忙概括或讨论书里的内容，推荐相关的书。医疗、法律、财务问题只提供一般信息，并建议咨询专业人士。" \
    "没听清或意思不完整时，请对方再说一遍，不要猜。"

typedef enum { CMD_START, CMD_STOP, CMD_TOGGLE, CMD_CONFIGURE, CMD_WS_END, CMD_WARM, CMD_COOL } cmd_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmds;
static TaskHandle_t s_ctl;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR ai_status_t s_st;

static esp_websocket_client_handle_t s_ws;
// 连接代号：每次连接换一个，事件回调带着它；关连接时清零，旧连接迟到的事件一律忽略，不会碰新连接
// 或已释放的缓冲。s_handlers 是正在回调里的个数，释放缓冲前要等它归零。
// Connection generation: a new one per connection, carried by its event callbacks; cleared on close,
// so late events from an old connection are ignored and never touch a new connection or freed
// buffers. s_handlers counts callbacks in progress; buffers are freed only once it is zero.
static _Atomic unsigned s_ws_gen;
static unsigned s_ws_gen_next;
static _Atomic int s_handlers;
static _Atomic int s_reapers;       // 还在收尾的旧连接 / old connections still shutting down
// 会话已结束、但旧连接的任务还没退出，WiFi 留到它退出再收回（见 session_end）。
// The chat has ended but an old connection's task has not exited yet; WiFi is released once it has
// (see session_end).
static _Atomic bool s_release_pending;
static int64_t s_release_pending_us;  // 开始等的时刻 / when the wait began
#define RELEASE_WAIT_MS 30000            // 比连接超时（20 秒）长 / longer than the connect timeout (20 s)
// 预热：AI 页开着时，先在后台开好音频（I2S）和 WiFi，但不连 AI 服务；点开始就直接连服务。对话结束后
// 如果还停在这个页面，两样都留着方便接着问。没有对话的话 WARM_HOLD_MS 后收回，离开页面立即收回。
// I2S 必须先于 WiFi 打开（见 session_start），所以预热也按这个顺序。
// Warm-up: while the AI page is open, audio (I2S) and WiFi come up in the background without
// connecting to the AI service, so a tap on start goes straight to the service. After a chat, both
// stay while the page remains open for a follow-up. With no chat they are released after
// WARM_HOLD_MS, and at once on leaving the page. I2S must open before WiFi (see session_start), so the
// warm-up keeps that order.
#define WARM_HOLD_MS 90000
static _Atomic bool s_page_open;     // AI 页开着 / the AI page is open
static _Atomic bool s_warm;          // 音频与 WiFi 由预热持有（不在对话中）/ audio and WiFi held by the warm-up (no chat)
static int64_t s_warm_until_us;      // 到点收回 / release when due
static bool s_audio_open;            // 本模块打开的 I2S 还开着 / the I2S this module opened is still open
// 「正在连接」时点了取消、开始命令还排在队里：开始命令到了就作废。
// Cancel tapped while connecting with the start command still queued: drop it when it arrives.
static _Atomic bool s_start_cancelled;
static char *s_rx;                  // 一条完整事件 / one whole event
static bool s_rx_overflow;
static StreamBufferHandle_t s_play;
static size_t s_play_bytes;
static _Atomic bool s_session, s_streaming, s_responding;
// 打断后丢弃本段回答余下的语音，直到它结束或新回答开始。/ After an interrupt, drop the rest of the
// reply's audio until it ends or a new reply starts.
static _Atomic bool s_discard;
// 下行诊断：最近收到数据的时刻、帧位置、回调里正做什么（卡住时报出来）。
// Downlink diagnostics: when data last arrived, the frame position, and what the callback is doing
// (reported when stuck).
static _Atomic int64_t s_rx_last_us;
static _Atomic unsigned s_rx_events;
static volatile int s_rx_op, s_rx_off, s_rx_len, s_rx_total;
static const char *volatile s_ws_where = "idle";
static int64_t s_hold_us;           // 本段回答因缓冲满等待播放的总时长 / time this reply waited for buffer room
// 控制任务正在做的一步与开始时刻（播放任务据此报告卡死）。/ The control task's current step and its
// start (the playback task reports a hang from these).
static const char *volatile s_ctl_step;
static _Atomic int64_t s_ctl_step_us;
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
static bool s_ws_ever;               // 本次会话连上过 / connected at least once this session
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

// 状态锁只护几行拷贝，正常拿锁是瞬间的；等了 2 秒说明有人拿着它卡住了，记下是谁再接着等。
// The status lock guards a few lines of copying and is normally instant; a 2 s wait means its holder
// is stuck, so log who it is and keep waiting.
static void lock(void) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) == pdTRUE) return;
    const TaskHandle_t holder = xSemaphoreGetMutexHolder(s_lock);
    ai_log("status lock: %s waited 2 s, held by %s", pcTaskGetName(NULL), holder ? pcTaskGetName(holder) : "?");
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { ++s_st.revision; xSemaphoreGive(s_lock); }
// 只在控制任务里调用。/ Control task only.
static void ctl_step(const char *step) {
    atomic_store(&s_ctl_step_us, esp_timer_get_time());
    s_ctl_step = step;
}
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

// 发送锁与客户端锁都只等 timeout_ms：发送最多阻塞这么久，关连接时也就最多等这么久。
// Both the send lock and the client lock are waited on for timeout_ms at most, which bounds how long a
// send blocks and so how long closing the connection waits for one.
static void send_text(char *text, size_t len, int timeout_ms) {
    const int64_t t0 = esp_timer_get_time();
    if (xSemaphoreTake(s_send_mtx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ai_log("send skipped: %u bytes, send lock busy for %d ms", (unsigned)len, timeout_ms);
        return;
    }
    const int left = timeout_ms - (int)((esp_timer_get_time() - t0) / 1000);
    if (s_ws && atomic_load(&s_ws_gen) && esp_websocket_client_is_connected(s_ws)) {
        const int sent = esp_websocket_client_send_text(s_ws, text, (int)len, pdMS_TO_TICKS(left > 1 ? left : 1));
        const int ms = (int)((esp_timer_get_time() - t0) / 1000);
        if (sent < 0) ai_log("send failed: %u bytes after %d ms (receive at %s)", (unsigned)len, ms, s_ws_where);
        else if (ms > 1000) ai_log("slow send: %u bytes took %d ms", (unsigned)len, ms);
    }
    xSemaphoreGive(s_send_mtx);
}

static void send_json(cJSON *root, int timeout_ms) {
    char id[16];
    snprintf(id, sizeof(id), "c%u", atomic_fetch_add(&s_event_id, 1) + 1);
    cJSON_AddStringToObject(root, "event_id", id);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text) {
        send_text(text, strlen(text), timeout_ms);
        cJSON_free(text);
    }
}

static void send_type(const char *type, int timeout_ms) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    send_json(root, timeout_ms);
}

static void send_session_update(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON *session = cJSON_AddObjectToObject(root, "session");
    cJSON *modalities = cJSON_AddArrayToObject(session, "modalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddStringToObject(session, "instructions", INSTRUCTIONS);
    // 官方文档示例把音色放在 session.voice；audio.output.voice 也照发，两处同值。音色只在首个 session.update 生效。
    // The official example puts the voice at session.voice; audio.output.voice is still sent with the same
    // value. The voice only takes effect in the first session.update.
    cJSON_AddStringToObject(session, "voice", RT_VOICE);
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
    send_json(root, SEND_TIMEOUT_MS);
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

// 一段语音放进回复缓冲。缓冲满时等播放腾出地方（每 20 ms 看一次），不丢：服务端发得比播放快，长回答
// 迟早会满。只按整样点（偶数字节）写：旧代码满时部分写入，曾留下奇数字节（日志“dropped 6785
// bytes”），之后整段回答错位一个字节成了噪声。只有这一个写入者，查到的空位不会变少，写入必定全进。
// 等待期间 TCP 自然限速；仍刷新 s_rx_last_us，免得被当成下行卡住。打断、换连接或会话结束时立即放弃。
// 返回是否全部放入。
// Queue one chunk of reply audio. When the buffer is full, wait for playback to make room (checking
// every 20 ms) instead of dropping: the server outpaces playback, so a long reply fills it sooner or
// later. Only whole samples (even byte counts) are written: the old code wrote part of a chunk when
// full and once left an odd byte count (logged "dropped 6785 bytes"), shifting the rest of the reply
// by one byte into noise. This is the only writer, so the free space found cannot shrink and the write
// always goes in whole. TCP throttles naturally while waiting; s_rx_last_us is still refreshed so this
// is not taken for a stuck downlink. Gives up at once on an interrupt, a new connection or the end of
// the session. Returns whether everything was queued.
static bool queue_reply(const int16_t *pcm, int n, unsigned gen) {
    const uint8_t *bytes = (const uint8_t *)pcm;
    const size_t total = (size_t)n * sizeof(int16_t);
    size_t done = 0;
    int64_t t0 = 0;
    while (done < total) {
        size_t room = xStreamBufferSpacesAvailable(s_play) & ~(size_t)1;
        if (room > total - done) room = total - done;
        if (room) {
            done += xStreamBufferSend(s_play, bytes + done, room, 0);
            continue;
        }
        if (atomic_load(&s_discard) || !atomic_load(&s_session) || atomic_load(&s_ws_gen) != gen) break;
        if (!t0) {
            t0 = esp_timer_get_time();
            s_ws_where = "waiting for buffer room";
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        atomic_store(&s_rx_last_us, esp_timer_get_time());
    }
    if (t0) {
        s_hold_us += esp_timer_get_time() - t0;
        s_ws_where = "audio";
    }
    return done >= total;
}

static void play_audio_event(char *text, unsigned gen) {
    // 打断后本段回答余下的语音直接丢掉，连解码都省了。/ After an interrupt the rest of the reply is dropped undecoded.
    if (atomic_load(&s_discard)) return;
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
    // 打断或断开；放进去之后才被打断的那一点由播放任务倒掉。/ Interrupted or disconnected; a chunk
    // queued just before an interrupt is poured away by the playback task.
    if (!queue_reply(pcm, m, gen) || atomic_load(&s_discard)) return;
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
        atomic_store(&s_discard, false);  // 新回答：打断的那段已过去 / a new reply: the interrupted one is over
        atomic_store(&s_responding, true);
        s_hold_us = 0;
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
        // 被打断的回答结束了：界面早已回到“在听”，用户可能正说着，不改状态。
        // The interrupted reply has ended: the page is already back to listening and the user may be
        // speaking, so the state is left alone.
        const bool cancelled = atomic_exchange(&s_discard, false);
        // 没有语音的回答（没听清等）：回到等待说话，否则麦克风一直不听。
        // A reply without audio (e.g. not understood): back to listening, or the mic would stay deaf.
        if (!cancelled && (s_st.state == AI_THINKING || s_st.state == AI_HEARING)) set_state(AI_LISTENING);
        // 收到的语音时长 ÷ 接收用时：大于 1 说明网络与 TLS 解密都跟得上播放。
        // Audio received ÷ time taken to receive it: above 1 means network and TLS decryption outpace playback.
        const int audio_ms = (int)(s_reply_samples * 1000 / AUDIO_RATE);
        const int recv_ms = s_reply_first_us ? (int)((s_reply_last_us - s_reply_first_us) / 1000) : 0;
        ai_log("response.done%s: reply audio %d ms received in %d ms (%.1fx realtime), waited %d ms for buffer "
               "room; internal=%u dma=%u",
               cancelled ? " (interrupted)" : "", audio_ms, recv_ms, recv_ms > 0 ? (double)audio_ms / recv_ms : 0.0,
               (int)(s_hold_us / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
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

static void lw_log(const char *why);
static void pkt_dump(const char *why);
static void ws_event_live(int32_t id, esp_websocket_event_data_t *ev, unsigned gen) {
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        atomic_store(&s_ws_connected, true);
        s_ws_ever = true;
        atomic_store(&s_rx_last_us, esp_timer_get_time());
        ai_log("ws connected; internal=%u dma=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        lw_log("connect ok");
        pkt_dump("connect ok");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 8) {  // 服务端关闭帧：2 字节关闭码 + 原因 / server close frame: 2-byte code + reason
            const int code = ev->data_len >= 2 ? ((uint8_t)ev->data_ptr[0] << 8 | (uint8_t)ev->data_ptr[1]) : 0;
            ai_log("server closed: code=%d reason=%.*s", code, ev->data_len > 2 ? ev->data_len - 2 : 0,
                   ev->data_len > 2 ? ev->data_ptr + 2 : "");
            snprintf(s_last_cause, sizeof(s_last_cause), "AI 服务关闭了连接（%d）", code);
            break;
        }
        // 记下帧位置：下行卡住时据此判断是不是卡在一帧中间。/ Note the frame position: when the downlink
        // sticks, this shows whether it stuck mid-frame.
        s_rx_op = ev->op_code;
        s_rx_off = ev->payload_offset;
        s_rx_len = ev->data_len;
        s_rx_total = ev->payload_len;
        atomic_fetch_add(&s_rx_events, 1);
        if (ev->data_len > 0) atomic_store(&s_rx_last_us, esp_timer_get_time());
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
            if (audio) {
                s_ws_where = "audio";
                touch();
                play_audio_event(s_rx, gen);
            } else {
                s_ws_where = "event";
                handle_event(s_rx);
            }
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

// 只处理当前连接的事件；关连接后旧连接迟到的事件直接丢掉（它的任务可能还在收尾）。
// Only the current connection's events are handled; late events from a closed one are dropped (its task
// may still be shutting down).
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)base;
    const unsigned gen = (unsigned)(uintptr_t)arg;
    atomic_fetch_add(&s_handlers, 1);  // 先登记再查代号，与关连接的顺序配对 / count first, then check: pairs with close
    if (gen == atomic_load(&s_ws_gen) && s_rx) {
        s_ws_where = "callback";
        ws_event_live(id, data, gen);
        s_ws_where = "library";
    }
    atomic_fetch_sub(&s_handlers, 1);
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
        // 最后一帧发送期间不算卡（队列已空，停滞检查只看这个时刻）。/ Not stuck while the last frame goes
        // out (the queue is already empty, so the stall check relies on this time).
        if (f->commit) atomic_store(&s_rx_last_us, esp_timer_get_time());
        // 重连期间积压的音频属于断掉的会话，直接丢弃。/ Audio queued across a reconnect belongs to the lost session; drop it.
        if (msg && atomic_load(&s_streaming)) {
            if (f->n > 0) {
                const int head = snprintf(msg, 96, "{\"event_id\":\"a%u\",\"type\":\"input_audio_buffer.append\",\"audio\":\"",
                                          atomic_fetch_add(&s_event_id, 1) + 1);
                size_t b64_len = 0;
                mbedtls_base64_encode((unsigned char *)msg + head, b64_cap, &b64_len, (const unsigned char *)f->pcm,
                                      (size_t)f->n * 2);
                memcpy(msg + head + b64_len, "\"}", 3);
                send_text(msg, (size_t)head + b64_len + 2, SEND_TIMEOUT_MS);
            }
            if (f->commit) {
                send_type("input_audio_buffer.commit", SEND_TIMEOUT_MS);
                send_type("response.create", SEND_TIMEOUT_MS);
                // 从现在起等回答：下行卡住的计时由此开始。/ Waiting for the reply from now: the stall clock starts here.
                atomic_store(&s_rx_last_us, esp_timer_get_time());
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
            // 等回答的计时从这里起（服务端在用户说话时不发数据，上次收到可能已是很久以前）。
            // The reply-wait clock starts here (the server sends nothing while the user talks, so the last
            // data may be long ago).
            atomic_store(&s_rx_last_us, esp_timer_get_time());
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

// 控制任务一步卡了 CTL_STUCK_MS 以上：报出是哪一步、它和 WebSocket 任务的状态，每步只报一次。由
// 播放任务调用（会话期间一直在跑，且不依赖控制任务）。
// The control task has been in one step for over CTL_STUCK_MS: report which step, and its and the
// WebSocket task's state, once per step. Called by the playback task, which runs throughout the
// session and does not depend on the control task.
static void log_task_states(const char *why);
static void check_ctl_stuck(void) {
    static int64_t reported_us;
    const char *step = s_ctl_step;
    const int64_t since = atomic_load(&s_ctl_step_us);
    if (!step || since == reported_us || esp_timer_get_time() - since < (int64_t)CTL_STUCK_MS * 1000) return;
    reported_us = since;
    ai_log("control task stuck %d ms in: %s; receive at %s", (int)((esp_timer_get_time() - since) / 1000), step,
           s_ws_where);
    log_task_states("stuck");
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
        check_ctl_stuck();
        if (atomic_load(&s_discard)) {
            // 打断：倒掉还没播的语音（读端随时可以安全地读空）。/ Interrupted: pour away unplayed audio
            // (the reader can always drain safely).
            while (xStreamBufferReceive(s_play, buf, MIC_SAMPLES * sizeof(int16_t), 0) > 0) {}
            if (primed) underflows += metalio_audio_tx_underflows() - underflow_mark;
            primed = false;
        }
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
    ai_log("mem %s: internal=%u dma=%u dma_largest=%u psram=%u psram_largest=%u", where,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/*
 * 内存测量：用数字确认每次对话把内存还回来、高峰时还有余量，而不是靠推断。
 *   - 最低余量：堆自己记的“开机以来最低值”是精确的；对话开始时记一次，结束时比一比，就知道这次对话
 *     有没有把它压得更低、压到多少（WiFi 动态接收缓冲、TLS、lwIP 都优先放 PSRAM，这里能看出来）。
 *   - 最大 DMA 空块没有现成的最低值，控制任务每圈（至多 200 ms）采一次。
 *   - 对账：本次开机第一次联网会创建 lwIP 的 TCP/IP 任务（栈 3 KB，在内部 RAM），lwIP 不支持卸载，它
 *     永远不释放，所以第一次对话会少约 5 KB；之后每次对话应回到第一次结束时的水平，偏差持续变大才是泄漏。
 * Memory measurement, to confirm with numbers (not inference) that each chat returns its memory and
 * leaves headroom at the peak:
 *   - Lowest headroom: the heap's own "lowest since boot" is exact; record it when the chat starts and
 *     compare at the end to see whether, and how far, this chat pushed it lower (WiFi dynamic receive
 *     buffers, TLS and lwIP all prefer PSRAM, and this shows it).
 *   - The largest DMA block has no low-water mark, so the control task samples it every loop (≤200 ms).
 *   - Audit: the first network use of a boot creates lwIP's TCP/IP task (3 KB stack in internal RAM),
 *     which is never freed because lwIP cannot be deinitialised, so the first chat ends about 5 KB short;
 *     later chats should return to where the first one ended, and only a drift that keeps growing is a leak.
 */
static bool s_tcpip_existed;      // 对话开始时 TCP/IP 任务已在 / the TCP/IP task existed when the chat started
static size_t s_mem_dma_first_end;  // 本次开机第一次对话结束后的 DMA 余量 / DMA headroom after this boot's first chat
static size_t s_min_internal_start, s_min_psram_start, s_low_dma_largest;

static void mem_session_begin(void) {
    s_tcpip_existed = xTaskGetHandle("tcpip") != NULL;  // lwIP 的 TCPIP_THREAD_NAME / lwIP's TCPIP_THREAD_NAME
    s_min_internal_start = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_min_psram_start = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    s_low_dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

static void mem_sample(void) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (largest < s_low_dma_largest) s_low_dma_largest = largest;
}

static void log_mem_low(void) {
    const size_t internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    ai_log("mem low (lowest since boot): internal=%u%s psram=%u%s; dma_largest sampled low=%u", (unsigned)internal,
           internal < s_min_internal_start ? " (set in this chat)" : " (before this chat)", (unsigned)psram,
           psram < s_min_psram_start ? " (set in this chat)" : " (before this chat)", (unsigned)s_low_dma_largest);
}

// 内存对账：WiFi 收回之后，余量应回到对话开始前的水平。/ Memory audit: once WiFi is released, headroom should be back where the chat started.
static void mem_audit(void) {
    const size_t dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    char note[96] = "";
    if (!s_tcpip_existed)
        snprintf(note, sizeof(note), "; first network use since boot (TCP/IP task kept for good)");
    else if (s_mem_dma_first_end)
        snprintf(note, sizeof(note), "; vs first chat end %+d", (int)dma - (int)s_mem_dma_first_end);
    if (!s_mem_dma_first_end) s_mem_dma_first_end = dma;
    ai_log("mem audit: dma %u -> %u (%+d), dma_largest %u -> %u (%+d)%s%s", (unsigned)s_mem_dma_before, (unsigned)dma,
           (int)dma - (int)s_mem_dma_before, (unsigned)s_mem_dma_largest_before, (unsigned)largest,
           (int)largest - (int)s_mem_dma_largest_before,
           s_tcpip_existed && (int)dma + 512 < (int)s_mem_dma_before ? "  <-- NOT RETURNED" : "", note);
}

static void release_network(void) {
    atomic_store(&s_release_pending, false);
    media_net_release_if_unused();
    mem_audit();
}

// 诊断：连接前先解析一次域名，记下解析到的地址、用时和 DNS 服务器。lwIP 会缓存结果，随后 TLS 连接用的
// 正是这个地址：用来查首连总超时、重连又能连上，是不是每次落到了不同的服务器地址。
// Diagnostics: resolve the host before connecting and log the address, the time taken and the DNS
// server. lwIP caches the answer, so the TLS connect that follows uses this very address: this shows
// whether first connects that time out, while a retry succeeds, land on different server addresses.
static char s_server_ip[48];  // 最近解析到的 AI 服务器地址 / the AI server address last resolved

static void log_dns(void) {
    const int64_t t0 = esp_timer_get_time();
    const struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    const int err = getaddrinfo(RT_HOST, NULL, &hints, &res);
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    char ip[48] = "-";
    if (!err && res) {
        if (res->ai_family == AF_INET)
            inet_ntop(AF_INET, &((const struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof(ip));
        else if (res->ai_family == AF_INET6)
            inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)res->ai_addr)->sin6_addr, ip, sizeof(ip));
    }
    if (res) freeaddrinfo(res);
    if (!err) snprintf(s_server_ip, sizeof(s_server_ip), "%s", ip);
    char server[48] = "-";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t dns;
    if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
        snprintf(server, sizeof(server), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    ai_log("dns %s -> %s (err %d) in %d ms via %s", RT_HOST, ip, err, ms, server);
}

/*
 * 诊断：连不上或卡住时，同时向网关、两个公共 DNS（TCP 53）和 AI 服务器发起 TCP 连接，最多等 3 秒，
 * 分清是本机/WiFi 收发出了问题（连网关都不通）、整个外网不通，还是只有到 AI 服务器的路被挡。对方
 * 回“拒绝”（RST）也说明路是通的。同时打出 WiFi 驱动的缓冲与收发计数，看有没有因缓冲不够丢帧。
 * Diagnostics: when connecting fails or the link stalls, open TCP connections to the gateway, two
 * public DNS servers (TCP 53) and the AI server at once, waiting up to 3 s, to tell apart a local
 * WiFi/stack problem (even the gateway is unreachable), no internet at all, and a path blocked only to
 * the AI server. A "refused" (RST) also proves the path works. The WiFi driver's buffer and RX/TX
 * counters are dumped as well, to show frames dropped for want of buffers.
 */
/*
 * 诊断：向网关和两个公共 DNS 直接发 UDP 域名查询（不经本机缓存），最多等 2 秒。与 TCP 探测对照：
 * UDP 也到不了外网，是所有外发数据都丢；UDP 能到、TCP 不能，是只有 TCP 出问题。同时记下本机 MAC。
 * Diagnostics: send a UDP DNS query straight to the gateway and two public DNS servers (bypassing the
 * local cache), waiting up to 2 s. Compared with the TCP probe: if UDP cannot reach the internet either,
 * all outgoing traffic is lost; if UDP gets through and TCP does not, only TCP is affected. The station
 * MAC is logged too.
 */
/*
 * 诊断：lwIP 收发计数。连接开始时记一份，失败时打出这段时间的增量，直接看出：链路层发了多少、收了多少、
 * 驱动拒发（link err）多少；TCP 发出的段（含重发的连接请求）、收到的段，以及因校验错、协议错、无对应
 * 连接等被丢的段。据此分清是请求没发出去、回应没到，还是回应到了被本机丢掉。
 * Diagnostics: lwIP counters. A copy is taken when connecting starts and the increase is logged on
 * failure, showing directly: link frames sent and received and those the driver refused (link err); TCP
 * segments sent (connect requests and their retries included) and received, and those dropped for a bad
 * checksum, a protocol error, no matching connection and so on. This tells apart requests that never
 * left, replies that never came, and replies that came but were dropped here.
 */
#if LWIP_STATS
static struct stats_ lw_base;
static void lw_mark(void) { lw_base = lwip_stats; }
static void lw_log(const char *why) {
    const struct stats_ *n = &lwip_stats, *o = &lw_base;
#define D(f) (unsigned)(STAT_COUNTER)(n->f - o->f)
    ai_log("lwip (%s): link tx %u rx %u drop %u err %u mem %u | arp tx %u rx %u drop %u | ip tx %u rx %u drop %u "
           "chk %u rt %u", why, D(link.xmit), D(link.recv), D(link.drop), D(link.err), D(link.memerr), D(etharp.xmit),
           D(etharp.recv), D(etharp.drop), D(ip.xmit), D(ip.recv), D(ip.drop), D(ip.chkerr), D(ip.rterr));
    ai_log("lwip (%s): tcp tx %u rx %u drop %u chk %u proto %u rt %u mem %u err %u len %u | udp tx %u rx %u drop %u",
           why, D(tcp.xmit), D(tcp.recv), D(tcp.drop), D(tcp.chkerr), D(tcp.proterr), D(tcp.rterr), D(tcp.memerr),
           D(tcp.err), D(tcp.lenerr), D(udp.xmit), D(udp.recv), D(udp.drop));
#undef D
}
#else
static void lw_mark(void) {}
static void lw_log(const char *why) { (void)why; }
#endif

/*
 * 诊断：WiFi 收发口的包记录。包住 STA 网口的 input/linkoutput，把每个 TCP 与 ICMP 包（方向、对端、
 * 标志、序号、长度、驱动发送结果）记进环形表，连接失败或成功时打出来，看清是哪一个包没回来。记录只做
 * 拷贝几个字段，不打日志（收包在 WiFi 任务里，栈小）。
 * Diagnostics: a packet log at the WiFi port. The STA netif's input/linkoutput are wrapped and every TCP
 * and ICMP packet (direction, peer, flags, sequence, length, driver send result) goes into a ring, logged
 * when a connect fails or succeeds, to see exactly which packet never came back. Recording only copies a
 * few fields and never logs (receiving runs in the WiFi task, whose stack is small).
 */
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
typedef struct {
    int64_t us;
    uint32_t peer, seq, ack;
    uint16_t lport, rport, len;
    uint8_t out, proto, flags;  // proto 6 = TCP, 1 = ICMP（flags 为类型）/ (flags holds the type)
    int8_t err;
} pkt_t;
#define PKT_RING 96
static EXT_RAM_BSS_ATTR pkt_t s_pkts[PKT_RING];
static unsigned s_pkt_head;
static portMUX_TYPE s_pkt_mux = portMUX_INITIALIZER_UNLOCKED;
static struct netif *s_pkt_netif;
static netif_input_fn s_pkt_input;
// 最近一次收到 TCP 包的时间：推上行只在连接后还没收到过回应时做（见 uplink_nudge）。
// When a TCP packet last arrived: the uplink is pushed only while a connect has had no reply yet.
static volatile int64_t s_tcp_in_us;
static netif_linkoutput_fn s_pkt_output;
static int64_t s_pkt_t0;

static void pkt_note(struct pbuf *p, bool out, err_t err) {
    if (!p || p->len < 14 + 20) return;
    const uint8_t *e = p->payload;
    if (e[12] != 0x08 || e[13] != 0x00) return;  // 只看 IPv4 / IPv4 only
    const uint8_t *ip = e + 14;
    const unsigned ihl = (ip[0] & 0x0f) * 4u;
    const uint8_t proto = ip[9];
    if ((proto != 6 && proto != 1) || p->len < 14 + ihl + (proto == 6 ? 20u : 4u)) return;
    const uint8_t *l4 = ip + ihl;
    pkt_t r = {.us = esp_timer_get_time(), .out = out, .proto = proto, .err = (int8_t)err};
    if (!out && proto == 6) s_tcp_in_us = r.us;
    memcpy(&r.peer, out ? ip + 16 : ip + 12, 4);
    const unsigned total = (unsigned)ip[2] << 8 | ip[3];
    if (proto == 6) {
        const unsigned doff = (l4[12] >> 4) * 4u;
        r.lport = out ? (uint16_t)(l4[0] << 8 | l4[1]) : (uint16_t)(l4[2] << 8 | l4[3]);
        r.rport = out ? (uint16_t)(l4[2] << 8 | l4[3]) : (uint16_t)(l4[0] << 8 | l4[1]);
        r.seq = (uint32_t)l4[4] << 24 | (uint32_t)l4[5] << 16 | (uint32_t)l4[6] << 8 | l4[7];
        r.ack = (uint32_t)l4[8] << 24 | (uint32_t)l4[9] << 16 | (uint32_t)l4[10] << 8 | l4[11];
        r.flags = l4[13];
        r.len = total > ihl + doff ? (uint16_t)(total - ihl - doff) : 0;
    } else {
        if (l4[0] == 3 && l4[1] == 3) return;  // 端口不可达（推包的回应），不记 / port unreachable (nudge replies): skipped
        r.flags = l4[0];             // ICMP 类型 / ICMP type
        r.lport = l4[1];             // ICMP 代码 / ICMP code
        r.len = (uint16_t)total;
    }
    taskENTER_CRITICAL(&s_pkt_mux);
    s_pkts[s_pkt_head++ % PKT_RING] = r;
    taskEXIT_CRITICAL(&s_pkt_mux);
}

static err_t pkt_input(struct pbuf *p, struct netif *inp) {
    pkt_note(p, false, ERR_OK);
    return s_pkt_input(p, inp);
}

static err_t pkt_output(struct netif *netif, struct pbuf *p) {
    const err_t err = s_pkt_output(netif, p);
    pkt_note(p, true, err);
    return err;
}

// WiFi 起来后调用：网口每次启动 WiFi 都是新建的，所以每次都要重新包。
// Call once WiFi is up: the netif is created anew on every WiFi start, so wrap it every time.
static void pkt_trace_attach(void) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    struct netif *n = sta ? esp_netif_get_netif_impl(sta) : NULL;
    if (!n || n->input == pkt_input) return;
    s_pkt_netif = n;
    s_pkt_input = n->input;
    s_pkt_output = n->linkoutput;
    n->linkoutput = pkt_output;
    n->input = pkt_input;
    s_pkt_t0 = esp_timer_get_time();
    taskENTER_CRITICAL(&s_pkt_mux);
    s_pkt_head = 0;
    taskEXIT_CRITICAL(&s_pkt_mux);
}

static void pkt_dump(const char *why) {
    if (!s_pkt_netif) return;
    taskENTER_CRITICAL(&s_pkt_mux);
    const unsigned head = s_pkt_head;
    taskEXIT_CRITICAL(&s_pkt_mux);
    const unsigned n = head < PKT_RING ? head : PKT_RING;
    ai_log("packets (%s): %u logged, last %u; time is ms since WiFi came up", why, head, n);
    for (unsigned i = head - n; i != head; ++i) {
        const pkt_t r = s_pkts[i % PKT_RING];
        char peer[16];
        inet_ntop(AF_INET, &r.peer, peer, sizeof(peer));
        const int ms = (int)((r.us - s_pkt_t0) / 1000);
        if (r.proto == 6) {
            char f[8] = "";
            size_t k = 0;
            if (r.flags & 0x02) f[k++] = 'S';
            if (r.flags & 0x10) f[k++] = 'A';
            if (r.flags & 0x08) f[k++] = 'P';
            if (r.flags & 0x01) f[k++] = 'F';
            if (r.flags & 0x04) f[k++] = 'R';
            f[k] = 0;
            ai_log("  %6d %s tcp %s:%u lport %u [%s] seq %u ack %u len %u%s%s", ms, r.out ? "out" : "in ", peer,
                   r.rport, r.lport, f, (unsigned)r.seq, (unsigned)r.ack, r.len, r.out && r.err ? " send-err " : "",
                   r.out && r.err ? lwip_strerr(r.err) : "");
        } else {
            ai_log("  %6d %s icmp %s type %u code %u len %u", ms, r.out ? "out" : "in ", peer, r.flags, r.lport, r.len);
        }
    }
}

static void udp_probe(const char *gateway) {
    // www.aliyun.com 的 A 记录查询，ID 0x4b31，要求递归 / A query for www.aliyun.com, ID 0x4b31, recursion desired
    static const uint8_t query[] = {0x4b, 0x31, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    3, 'w', 'w', 'w', 6, 'a', 'l', 'i', 'y', 'u', 'n', 3, 'c', 'o', 'm', 0,
                                    0x00, 0x01, 0x00, 0x01};
    const char *ips[3] = {gateway, "223.5.5.5", "114.114.114.114"};
    const char *names[3] = {"gateway", "alidns", "114dns"};
    int fds[3] = {-1, -1, -1}, ms[3] = {0};
    const char *res[3] = {"skipped", "skipped", "skipped"};
    const int64_t t0 = esp_timer_get_time();
    int pending = 0;
    for (int i = 0; i < 3; ++i) {
        if (!ips[i]) continue;
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53)};
        if (inet_pton(AF_INET, ips[i], &addr.sin_addr) != 1) continue;
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { res[i] = "no socket"; continue; }
        if (sendto(fd, query, sizeof(query), 0, (struct sockaddr *)&addr, sizeof(addr)) != (int)sizeof(query)) {
            res[i] = "send error";
            close(fd);
            continue;
        }
        fds[i] = fd;
        res[i] = "timeout";
        ++pending;
    }
    while (pending > 0) {
        const int64_t left_us = 2000000 - (esp_timer_get_time() - t0);
        if (left_us <= 0) break;
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        for (int i = 0; i < 3; ++i)
            if (fds[i] >= 0) {
                FD_SET(fds[i], &rd);
                if (fds[i] > maxfd) maxfd = fds[i];
            }
        struct timeval tv = {.tv_sec = (time_t)(left_us / 1000000), .tv_usec = (suseconds_t)(left_us % 1000000)};
        if (select(maxfd + 1, &rd, NULL, NULL, &tv) <= 0) break;
        for (int i = 0; i < 3; ++i) {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &rd)) continue;
            uint8_t reply[64];
            const int n = recv(fds[i], reply, sizeof(reply), 0);
            res[i] = n >= 12 && reply[0] == 0x4b && reply[1] == 0x31 ? "answered" : "bad reply";
            ms[i] = (int)((esp_timer_get_time() - t0) / 1000);
            close(fds[i]);
            fds[i] = -1;
            --pending;
        }
    }
    for (int i = 0; i < 3; ++i)
        if (fds[i] >= 0) close(fds[i]);
    uint8_t mac[6] = {0};
    (void)esp_wifi_get_mac(WIFI_IF_STA, mac);
    ai_log("udp probe: %s %s (%d ms), %s %s (%d ms), %s %s (%d ms); sta mac %02x:%02x:%02x:%02x:%02x:%02x", names[0],
           res[0], ms[0], names[1], res[1], ms[1], names[2], res[2], ms[2], mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
}

static void net_probe(const char *why) {
    lw_log("since connect start");
    pkt_dump(why);
    struct {
        const char *name;
        char ip[48];
        uint16_t port;
        int fd;
        const char *result;
        int ms;
    } p[4] = {0};
    int n = 0;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.gw.addr) {
        p[n].name = "gateway";
        snprintf(p[n].ip, sizeof(p[n].ip), IPSTR, IP2STR(&info.gw));
        p[n++].port = 80;
    }
    p[n].name = "alidns";
    snprintf(p[n].ip, sizeof(p[n].ip), "223.5.5.5");
    p[n++].port = 53;
    p[n].name = "114dns";
    snprintf(p[n].ip, sizeof(p[n].ip), "114.114.114.114");
    p[n++].port = 53;
    if (s_server_ip[0] && !strchr(s_server_ip, ':')) {
        p[n].name = "ai-server";
        snprintf(p[n].ip, sizeof(p[n].ip), "%s", s_server_ip);
        p[n++].port = 443;
    }
    const int64_t t0 = esp_timer_get_time();
    int pending = 0;
    for (int i = 0; i < n; ++i) {
        p[i].fd = -1;
        p[i].result = "no socket";
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(p[i].port)};
        if (inet_pton(AF_INET, p[i].ip, &addr.sin_addr) != 1) { p[i].result = "bad address"; continue; }
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            p[i].result = "ok";
            close(fd);
            continue;
        }
        if (errno != EINPROGRESS) {
            p[i].result = errno == ECONNREFUSED ? "refused" : "error";
            close(fd);
            continue;
        }
        p[i].fd = fd;
        p[i].result = "timeout";
        ++pending;
    }
    while (pending > 0) {
        const int64_t left_us = 3000000 - (esp_timer_get_time() - t0);
        if (left_us <= 0) break;
        fd_set wr;
        FD_ZERO(&wr);
        int maxfd = -1;
        for (int i = 0; i < n; ++i)
            if (p[i].fd >= 0) {
                FD_SET(p[i].fd, &wr);
                if (p[i].fd > maxfd) maxfd = p[i].fd;
            }
        struct timeval tv = {.tv_sec = (time_t)(left_us / 1000000), .tv_usec = (suseconds_t)(left_us % 1000000)};
        if (select(maxfd + 1, NULL, &wr, NULL, &tv) <= 0) break;
        for (int i = 0; i < n; ++i) {
            if (p[i].fd < 0 || !FD_ISSET(p[i].fd, &wr)) continue;
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            getsockopt(p[i].fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
            p[i].result = soerr == 0 ? "ok" : soerr == ECONNREFUSED ? "refused" : soerr == ECONNRESET ? "reset" : "error";
            p[i].ms = (int)((esp_timer_get_time() - t0) / 1000);
            close(p[i].fd);
            p[i].fd = -1;
            --pending;
        }
    }
    for (int i = 0; i < n; ++i)
        if (p[i].fd >= 0) close(p[i].fd);
    wifi_ap_record_t ap;
    const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    char line[300];
    size_t len = 0;
    for (int i = 0; i < n && len < sizeof(line) - 64; ++i)
        len += (size_t)snprintf(line + len, sizeof(line) - len, " %s %s:%u %s", p[i].name, p[i].ip, p[i].port,
                                p[i].result);
    ai_log("net probe (%s): rssi %d;%s", why, rssi, line);
    for (int i = 0; i < n; ++i)
        if (p[i].ms) ai_log("  %s answered in %d ms", p[i].name, p[i].ms);
    udp_probe(p[0].name && !strcmp(p[0].name, "gateway") ? p[0].ip : NULL);
    lw_log("since connect start, probes included");
    esp_wifi_statis_dump(WIFI_STATIS_BUFFER | WIFI_STATIS_RXTX | WIFI_STATIS_DIAG);
}

// 诊断：WebSocket 与控制任务此刻的状态（运行/就绪/阻塞）、当前优先级（含继承来的）和栈余量。只读
// 系统任务表，不碰可能已删除的任务句柄。
// Diagnostics: the WebSocket and control tasks' current state (running/ready/blocked), current
// priority (inherited included) and stack headroom. Reads the system task table only and never
// touches a handle whose task may be gone.
static void log_task_states(const char *why) {
#if configUSE_TRACE_FACILITY
    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    TaskStatus_t *tasks = heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tasks) return;
    const UBaseType_t n = uxTaskGetSystemState(tasks, cap, NULL);
    static const char *const names[] = {"websocket_task", "ai_ctl", "ai_reap", "ai_play", "main"};
    static const char states[] = "RrBSD?";
    char line[200];
    size_t len = 0;
    for (UBaseType_t i = 0; i < n && len < sizeof(line) - 48; ++i) {
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); ++k) {
            if (strcmp(tasks[i].pcTaskName, names[k])) continue;
            const int st = tasks[i].eCurrentState <= eDeleted ? (int)tasks[i].eCurrentState : 5;
            len += (size_t)snprintf(line + len, sizeof(line) - len, " %s=%c/p%u/stack%u", names[k], states[st],
                                    (unsigned)tasks[i].uxCurrentPriority, (unsigned)tasks[i].usStackHighWaterMark);
        }
    }
    heap_caps_free(tasks);
    ai_log("tasks (%s):%s", why, len ? line : " none found");
#else
    (void)why;
#endif
}

/*
 * 实验（2026-11-24）：连接期间推上行。抓包显示，WiFi 拿到地址约 0.3 秒后，本机发出的包（含发往网关的）
 * 全部丢失约 15 秒，收包一直正常；而一旦集中发出一批包（连接失败后的网络探测），紧接着的重连每次都立刻
 * 成功。推断：加密包序号在连上后被重置，对端把序号偏小的包当重放丢弃，直到本机发出的包数追上旧序号。
 * 验证：连接未完成时每秒向网关发 30 个 1 字节 UDP 包（丢弃端口 9）。推断成立则连接应在一两秒内打通。
 * Experiment (2026-11-24): push the uplink while connecting. Packet logs show that about 0.3 s after
 * WiFi gets its address, every packet this device sends (to the gateway too) is lost for about 15 s
 * while receiving works throughout; and as soon as a batch of packets goes out (the network probe after
 * a failed connect), the reconnect that follows succeeds at once, every time. Hypothesis: the encrypted
 * packet number is reset after joining, and the peer drops packets with a lower number as replays until
 * this device has sent enough to pass the old number. Test: while not yet connected, send 30 one-byte
 * UDP packets to the gateway's discard port (9) each second. If the hypothesis holds, connecting should
 * get through within a second or two.
 */
static int64_t s_connect_us, s_nudge_us;

static void uplink_nudge(void) {
    int sent = 0, failed = 0;
    media_net_uplink_nudge(&sent, &failed);
    ai_log("uplink nudge at %d ms into connect: %d sent, %d refused",
           (int)((esp_timer_get_time() - s_connect_us) / 1000), sent, failed);
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
        // 固定在核 0：在核 1 时与播放任务抢核，每段回答 I2S 放空 9–18 次；在核 0 时为 0。
        // Pinned to core 0: on core 1 it competed with playback and I2S underflowed 9–18 times per reply;
        // on core 0 it was 0.
        .task_core_id_set = true,
        .task_core_id = 0,
        .buffer_size = 4096,
        .network_timeout_ms = NET_TIMEOUT_MS,
    };
    s_last_cause[0] = 0;
    atomic_store(&s_ws_connected, false);
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) { snprintf(s_last_cause, sizeof(s_last_cause), "内存不足"); return false; }
    if (++s_ws_gen_next == 0) ++s_ws_gen_next;  // 0 表示没有连接 / 0 means no connection
    const unsigned gen = s_ws_gen_next;
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, (void *)(uintptr_t)gen);
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);  // 此时没有连接，没人在发 / no connection yet, so nobody is sending
    s_ws = ws;
    xSemaphoreGive(s_send_mtx);
    atomic_store(&s_rx_events, 0);
    atomic_store(&s_rx_last_us, esp_timer_get_time());
    s_ws_where = "library";
    atomic_store(&s_ws_gen, gen);
    s_connect_us = esp_timer_get_time();
    s_nudge_us = 0;
    log_dns();
    lw_mark();  // 计数从这里起，不含上面的域名解析 / counters start here, the lookup above excluded
    if (esp_websocket_client_start(ws) != ESP_OK) {
        snprintf(s_last_cause, sizeof(s_last_cause), "连接 AI 服务失败");
        return false;
    }
    log_mem("connecting");
    return true;
}

static void reap_task(void *arg) {
    esp_websocket_client_destroy(arg);
    atomic_fetch_sub(&s_reapers, 1);
    vTaskDeleteWithCaps(NULL);
}

/*
 * 关连接，控制任务最多等约 REAP_WAIT_MS，绝不无限期卡住。旧做法 esp_websocket_client_close() 发关闭帧
 * 时无限期等客户端锁，接收任务卡住时会一直占着这把锁，控制任务就可能永远卡在这里（打断后发不出
 * 取消、整整等了 8 秒，就是被这把锁挡住）。现在：
 *   1. 代号清零：旧连接的事件从此忽略；
 *   2. 强制关套接字：卡在上面的收发立即出错返回，接收任务随即退出；
 *   3. 拿发送锁摘下句柄：发送都有时限，最多等一次发送超时；
 *   4. 由收尾任务销毁并等它最多 REAP_WAIT_MS，还没完就让它自己收尾，控制任务继续。
 * Close the connection with the control task waiting about REAP_WAIT_MS at most, never forever. The
 * old esp_websocket_client_close() waited forever for the client lock to send its CLOSE frame, and a
 * stuck receive task holds that lock indefinitely, so the control task could hang here for good (the
 * cancel after an interrupt waited the full 8 s on this same lock). Now:
 *   1. clear the generation: the old connection's events are ignored from here on;
 *   2. force the socket closed: sends and receives blocked on it fail at once and the receive task exits;
 *   3. detach the handle under the send lock: every send is time-limited, so this waits one send timeout at most;
 *   4. destroy it in a helper task and wait up to REAP_WAIT_MS; if it is not done, it finishes on its
 *      own and the control task carries on.
 */
static void ws_close(void) {
    esp_websocket_client_handle_t ws = s_ws;  // 只有控制任务改它 / only the control task changes it
    atomic_store(&s_ws_gen, 0);
    atomic_store(&s_ws_connected, false);
    if (!ws) return;
    const int64_t t0 = esp_timer_get_time();
    ctl_step("ws_close: abort socket");
    const esp_err_t aborted = esp_websocket_client_abort_socket(ws);
    ctl_step("ws_close: detach");
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    s_ws = NULL;
    xSemaphoreGive(s_send_mtx);
    ctl_step("ws_close: destroy");
    atomic_fetch_add(&s_reapers, 1);
    if (xTaskCreatePinnedToCoreWithCaps(reap_task, "ai_reap", 4 * 1024, ws, 4, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_fetch_sub(&s_reapers, 1);
        esp_websocket_client_destroy(ws);  // 建不了收尾任务就在这里销毁 / no helper task: destroy here
    }
    while (atomic_load(&s_reapers) > 0 && esp_timer_get_time() - t0 < (int64_t)REAP_WAIT_MS * 1000)
        vTaskDelay(pdMS_TO_TICKS(10));
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (atomic_load(&s_reapers) > 0) {
        ai_log("ws close: task still running after %d ms (abort %s, receive at %s); left to finish on its own", ms,
               esp_err_to_name(aborted), s_ws_where);
        log_task_states("close");
    } else if (ms > 500) {
        ai_log("ws close took %d ms", ms);
    }
}

// 释放会话缓冲。只在工作任务与回调都已退出时调用。/ Free the chat's buffers; call only once workers and callbacks have exited.
static void free_session_buffers(void) {
    if (s_play) { psram_stream_delete(s_play); s_play = NULL; }  // 不用 IDF 6.1 有缺陷的 WithCaps 删除 / avoids the faulty IDF 6.1 WithCaps delete
    s_play_bytes = 0;
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

static void session_end(const char *err) {
    atomic_store(&s_streaming, false);
    atomic_store(&s_session, false);
    s_retry_at_us = 0;
    ai_log("session end: %s", err ? err : "(normal)");
    ws_close();
    ctl_step("session_end: workers");
    // 录音任务最多阻塞 200 ms，上传任务的发送已被 ws_close 等完。还在回调里的（旧连接收尾时可能有）
    // 也要等它出来，才能释放它用的接收与播放缓冲。
    // The mic task blocks at most 200 ms; ws_close already waited out any send in progress. A callback
    // still running (possible while an old connection shuts down) must also leave before the receive
    // and playback buffers it uses are freed.
    for (int i = 0; i < 150 && (atomic_load(&s_workers) > 0 || atomic_load(&s_handlers) > 0); ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    const bool workers_done = atomic_load(&s_workers) == 0 && atomic_load(&s_handlers) == 0;
    if (!workers_done)
        ai_log("workers %d / callbacks %d still running after 3 s: keeping their buffers", atomic_load(&s_workers),
               atomic_load(&s_handlers));
    ctl_step("session_end: release");
    // 页面还开着就留着音频和 WiFi，接着问不用重新连（见 WARM_HOLD_MS）。
    // With the page still open, audio and WiFi stay for a follow-up (see WARM_HOLD_MS).
    const bool keep = atomic_load(&s_page_open) && s_audio_open && workers_done;
    if (keep) {
        metalio_audio_set_pa(false);
    } else {
        metalio_audio_close();
        s_audio_open = false;
    }
    // 任务没退出就不释放它们还在用的缓冲：宁可漏一次内存，也不能写坏堆。
    // Buffers still in use by a live task are kept: leaking once beats corrupting the heap.
    if (workers_done) free_session_buffers();
    atomic_store(&s_responding, false);
    atomic_store(&s_discard, false);
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
    log_mem_low();
    // 旧连接的任务还没退出（它的套接字可能还压着 WiFi 驱动的接收缓冲）就先不停 WiFi：控制任务等它退出
    // 后再收回（见 ctl_task）。
    // While an old connection's task is still running (its socket may still hold WiFi driver receive
    // buffers), WiFi stays up; the control task releases it once that task exits (see ctl_task).
    if (keep) {
        atomic_store(&s_warm, true);
        s_warm_until_us = esp_timer_get_time() + (int64_t)WARM_HOLD_MS * 1000;
        ai_log("warm: keeping wifi + i2s for %d s while the page is open", WARM_HOLD_MS / 1000);
    } else if (atomic_load(&s_reapers) > 0) {
        s_release_pending_us = esp_timer_get_time();
        atomic_store(&s_release_pending, true);
        ai_log("network kept until the old connection's task exits");
    } else {
        release_network();
    }
    log_flush();
}

// 先开 I2S（麦克风+喇叭）再连 WiFi：I2S DMA 只能用内部 RAM，实测 WiFi 连上后最大空块只剩约 5.8 KB，
// 先连网时 I2S 一次也没分到过，每次都得断网、开 I2S、再连一遍，白等约 5 秒。WiFi 已被别的功能连着
// 而分不到时，才断网再开。已经开着（预热或上次对话留下）就直接成功。
// Open I2S (mic + speaker) before WiFi: I2S DMA must be internal RAM, and once WiFi is up the largest
// free block measured only ~5.8 KB. Connecting first never once left room for I2S, so every chat
// dropped WiFi, opened I2S and connected again, wasting ~5 s. Only when WiFi is already up for
// something else and I2S does not fit is WiFi dropped first. Already open (warm-up or the last chat)
// succeeds at once.
static bool open_audio(void) {
    if (metalio_audio_open(AUDIO_RATE, true) != ESP_OK) {
        ai_log("i2s did not fit; dropping wifi first");
        media_net_drop();
        if (metalio_audio_open(AUDIO_RATE, true) != ESP_OK) return false;
    }
    s_audio_open = true;
    return true;
}

static void warm_release(const char *why) {
    if (!atomic_load(&s_warm) || atomic_load(&s_session)) return;
    atomic_store(&s_warm, false);
    metalio_audio_close();
    s_audio_open = false;
    ai_log("warm: released (%s)", why);
    if (atomic_load(&s_reapers) > 0) {
        s_release_pending_us = esp_timer_get_time();
        atomic_store(&s_release_pending, true);
    } else {
        release_network();
    }
    log_flush();
}

// 预热时打通上行（见 media_net_ensure_uplink），让刚连上 WiFi 时的卡顿在用户看页面时过去。
// Open the uplink during warm-up (see media_net_ensure_uplink), so the stall right after joining
// WiFi passes while the user is looking at the page.
static bool page_still_open(void) { return atomic_load(&s_page_open); }

static void open_uplink(const char *why, bool (*keep)(void)) {
    const int ms = media_net_ensure_uplink(RT_HOST, 443, &s_cancel, keep);
    if (ms > 0) ai_log("%s: uplink opened in %d ms", why, ms);
    else if (ms < 0) ai_log("%s: uplink not confirmed", why);
}

static void warm_up(void) {
    if (!atomic_load(&s_page_open) || atomic_load(&s_session) || atomic_load(&s_warm)) return;
    if (!RT_API_KEY[0] || music_active()) return;
    if (atomic_load(&s_workers) > 0 || atomic_load(&s_handlers) > 0) return;  // 上次还在收尾 / last chat still winding down
    s_cancel = false;
    atomic_store(&s_release_pending, false);
    mem_session_begin();
    s_mem_dma_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_mem_dma_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const int64_t t0 = esp_timer_get_time();
    ctl_step("warm: audio");
    if (!open_audio()) { ai_log("warm: audio unavailable"); return; }
    atomic_store(&s_warm, true);
    s_warm_until_us = esp_timer_get_time() + (int64_t)WARM_HOLD_MS * 1000;
    const int64_t t_i2s = esp_timer_get_time();
    char err[64] = {0};
    ctl_step("warm: wifi");
    const bool ok = media_net_acquire(err, sizeof(err), &s_cancel);
    s_warm_until_us = esp_timer_get_time() + (int64_t)WARM_HOLD_MS * 1000;
    if (ok) ai_log("warm: wifi + i2s ready; audio %d ms, wifi %d ms", (int)((t_i2s - t0) / 1000),
                   (int)((esp_timer_get_time() - t_i2s) / 1000));
    else ai_log("warm: wifi not ready yet (%s)", s_cancel ? "cancelled" : err);
    if (ok) {
        ctl_step("warm: uplink");
        open_uplink("warm", page_still_open);
        s_warm_until_us = esp_timer_get_time() + (int64_t)WARM_HOLD_MS * 1000;
    }
    // 离开了页面就收回；还在页面上就留着，点开始时会接着等 WiFi。
    // Page left: release. Still on the page: keep everything; start will keep waiting for WiFi.
    if (!atomic_load(&s_page_open)) warm_release("page closed");
}

// 连接断了：还能重连就稍后重连（麦克风、喇叭、WiFi 都保留），否则结束会话。
// The connection dropped: reconnect shortly if retries remain (mic, speaker and WiFi stay up), else end.
static void ws_lost(void) {
    atomic_store(&s_streaming, false);
    ws_close();
    clear_playback();
    atomic_store(&s_responding, false);
    const char *cause = s_last_cause[0] ? s_last_cause : "连接已断开";
    if (!s_auth_failed) {
        ctl_step("ws_lost: network probe");
        net_probe(s_ws_ever ? "after drop" : "connect failed");
    }
    if (!s_auth_failed && s_retries < MAX_RETRIES) {
        ++s_retries;
        s_retry_at_us = esp_timer_get_time() + (int64_t)RETRY_DELAY_MS * 1000;
        ai_log("connection lost (%s); reconnect %d/%d in %d ms", cause, s_retries, MAX_RETRIES, RETRY_DELAY_MS);
        char msg[sizeof(s_st.error)];
        // 还没连上过就不是“中断”，只是这次没连上。/ Never connected yet: not a drop, just a failed attempt.
        snprintf(msg, sizeof(msg), s_ws_ever ? "连接中断，正在重连（%d/%d）…" : "连接较慢，正在重试（%d/%d）…", s_retries,
                 MAX_RETRIES);
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
    // 上次对话的工作任务或回调没在限时内退出时，session_end 留着它们的缓冲没放。这时再开会话，旧任务会
    // 随 s_session 变真重新干活、和新任务抢同一批缓冲（s_workers 计数也被覆盖），所以先不开。
    // If the last chat's workers or a callback did not exit in time, session_end kept their buffers.
    // Starting now would wake those old tasks (s_session turns true again) to share buffers with the new
    // ones and overwrite the s_workers count, so refuse until they are gone.
    if (atomic_load(&s_workers) > 0 || atomic_load(&s_handlers) > 0) {
        ai_log("start refused: last chat still has %d workers / %d callbacks running", atomic_load(&s_workers),
               atomic_load(&s_handlers));
        set_error("上次对话还在收尾，请稍后再试");
        set_state(AI_IDLE);  // ai_start 已先显示「正在连接」/ ai_start already showed "connecting"
        return;
    }
    if (atomic_exchange(&s_start_cancelled, false)) {
        ai_log("start dropped: cancelled while queued");
        set_state(AI_IDLE);
        return;
    }
    // 预热持有的音频和 WiFi 从这里起归这次对话。/ From here the warm audio and WiFi belong to this chat.
    const bool was_warm = atomic_exchange(&s_warm, false);
    // 上次留下的缓冲（任务当时没退出）现在可以放了，免得被下面的分配覆盖而漏掉、或沿用旧的回复缓冲。
    // Buffers kept last time (tasks had not exited then) can go now, rather than leak when overwritten
    // below or carry the old reply buffer over.
    if (s_rx || s_play || s_frames) {
        ai_log("freeing buffers kept from the last chat");
        free_session_buffers();
    }
    s_cancel = false;
    s_retries = 0;
    s_retry_at_us = 0;
    s_auth_failed = false;
    s_ws_ever = false;
    atomic_store(&s_discard, false);
    s_up_dropped = 0;
    s_reply_rate_req = REPLY_RATE_WANTED;
    s_reply_rate = REPLY_RATE_DEFAULT;
    lock();
    s_st.state = AI_CONNECTING;
    s_st.user[0] = s_st.reply[0] = s_st.error[0] = 0;
    s_st.mic_peak = 0;
    unlock();
    // 上次的旧连接还没退出：WiFi 留给这次对话用，不再等着收回。
    // An old connection from last time has not exited yet: WiFi stays for this chat, no release pending.
    atomic_store(&s_release_pending, false);
    if (!was_warm) {
        mem_session_begin();
        s_mem_dma_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_mem_dma_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    ai_log("=== session start; model %s%s", RT_MODEL, was_warm ? " (wifi + i2s already warm)" : "");
    log_mem("before");
    if (!RT_API_KEY[0]) { session_end("未配置 AI 服务密钥"); return; }
    // 音乐与 AI 不同时用音频和网络。/ Music and AI never share audio and network.
    if (music_active()) music_stop();
    char err[64] = {0};
    const int64_t t_start = esp_timer_get_time();
    // 先开 I2S 再连 WiFi（见 open_audio）。/ I2S before WiFi (see open_audio).
    if (!open_audio()) { session_end("音频设备不可用"); return; }
    const int64_t t_i2s = esp_timer_get_time();
    if (s_cancel) { session_end(NULL); return; }
    if (!media_net_acquire(err, sizeof(err), &s_cancel)) { session_end(s_cancel ? NULL : err); return; }
    ctl_step("start: uplink");
    open_uplink("start", NULL);  // 预热过就立即返回 / returns at once after a warm-up
    if (s_cancel) { session_end(NULL); return; }
    ai_log("wifi + i2s ready: i2s %d Hz measured %d; audio %d ms, wifi %d ms", metalio_audio_sample_rate(),
           metalio_audio_actual_rate(), (int)((t_i2s - t_start) / 1000), (int)((esp_timer_get_time() - t_i2s) / 1000));
    log_mem("wifi + i2s");
    pkt_trace_attach();
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
    for (size_t i = 0; !s_play && i < sizeof(k_play_ring_sizes) / sizeof(k_play_ring_sizes[0]); ++i) {
        s_play = psram_stream_create(k_play_ring_sizes[i], 1);
        s_play_bytes = s_play ? k_play_ring_sizes[i] : 0;
    }
    ai_log("reply buffer %u KB (%u s)", (unsigned)(s_play_bytes / 1024), (unsigned)(s_play_bytes / 2 / AUDIO_RATE));
    s_frames = heap_caps_malloc(UP_FRAMES * sizeof(up_frame_t), caps);
    s_up_free = xQueueCreateWithCaps(UP_FRAMES, sizeof(up_frame_t *), caps);
    s_up_ready = xQueueCreateWithCaps(UP_FRAMES, sizeof(up_frame_t *), caps);
    diag_alloc();  // 失败只是没有诊断 / failure only loses the diagnostics
    if (!s_rx || !s_rs_out || !s_rs_in || !s_play || !s_frames || !s_up_free || !s_up_ready) { session_end("内存不足"); return; }
    for (int i = 0; i < UP_FRAMES; ++i) {
        up_frame_t *f = &s_frames[i];
        xQueueSend(s_up_free, &f, 0);
    }
    ctl_step("start: connect");  // 等 WiFi 的时间不算卡（播放任务马上开始检查）/ the WiFi wait is not a hang (playback starts checking now)
    atomic_store(&s_session, true);
    atomic_store(&s_last_play_us, 0);
    touch();
    // 先起工作任务再连接：首次连接失败转入重连时它们也已就位（未就绪前它们什么都不做）。
    // Workers start before connecting, so they are in place even when the first attempt fails into a
    // reconnect (they idle until the session is ready).
    atomic_store(&s_workers, 3);
    if (xTaskCreatePinnedToCoreWithCaps(mic_task, "ai_mic", 6 * 1024, NULL, 6, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    if (xTaskCreatePinnedToCoreWithCaps(upload_task, "ai_up", 6 * 1024, NULL, 5, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    // 实测连同诊断日志最多用约 1.1 KB 栈。/ Measured at about 1.1 KB of stack at most, diagnostics included.
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "ai_play", 6 * 1024, NULL, 6, NULL, 1, caps) != pdPASS) atomic_fetch_sub(&s_workers, 1);
    if (atomic_load(&s_workers) != 3) { session_end("内存不足"); return; }
    if (!ws_open()) ws_lost();
}

// 打断：先在本地立即停下（丢弃本段回答余下的语音、关喇叭、回到“在听”），再尽力通知服务端取消，
// 发不出去也只等 CANCEL_TIMEOUT_MS。
// Interrupt: stop locally at once (drop the rest of the reply, speaker off, back to listening), then
// tell the server to cancel as best effort, waiting CANCEL_TIMEOUT_MS at most if it cannot be sent.
static void interrupt_reply(void) {
    atomic_store(&s_discard, true);  // 接收端丢弃、播放任务倒空缓冲 / receive drops, playback drains the buffer
    atomic_store(&s_responding, false);
    metalio_audio_set_pa(false);
    touch();
    set_state(AI_LISTENING);
    ai_log("interrupted by user (receive at %s)", s_ws_where);
    ctl_step("interrupt: send response.cancel");
    send_type("response.cancel", CANCEL_TIMEOUT_MS);
}

// 等回答期间（已提交、或回答进行中）久无数据：下行卡死，记下当时的帧位置与任务状态，按断线处理。
// Waiting for a reply (committed, or one in progress) with no data for long: the downlink is stuck.
// Log the frame position and task states at that moment, then treat it as a drop.
static void check_rx_stall(void) {
    if (!atomic_load(&s_session) || !s_ws || !atomic_load(&s_streaming)) return;
    if (!atomic_load(&s_responding) && s_st.state != AI_THINKING) return;
    // 话还没传完，服务端自然还没回答。/ Speech still uploading: no reply is expected yet.
    if (s_up_ready && uxQueueMessagesWaiting(s_up_ready) > 0) return;
    const int ms = (int)((esp_timer_get_time() - atomic_load(&s_rx_last_us)) / 1000);
    if (ms < RX_STALL_MS) return;
    ai_log("downlink stalled: no data for %d ms (%s); receive at %s, last frame op %d bytes %d+%d of %d, %u events",
           ms, atomic_load(&s_responding) ? "reply in progress" : "waiting for reply", s_ws_where, s_rx_op, s_rx_off,
           s_rx_len, s_rx_total, atomic_load(&s_rx_events));
    log_task_states("stall");
    snprintf(s_last_cause, sizeof(s_last_cause), "AI 回复中断（%d 秒没有收到数据）", ms / 1000);
    ws_lost();
}

static const char *const k_cmd_names[] = {"start", "stop", "toggle", "configure", "ws_end", "warm", "cool"};

static void ctl_task(void *arg) {
    (void)arg;
    cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
            const int64_t t0 = esp_timer_get_time();
            ctl_step(k_cmd_names[cmd]);
            switch (cmd) {
            case CMD_START: session_start(); break;
            // 会话可能已结束、音乐已接手音频：只在 AI 仍在进行时收尾。/ The chat may be over with music owning audio; end only a live chat.
            case CMD_STOP: if (atomic_load(&s_session) || s_st.state != AI_IDLE) session_end(NULL); break;
            case CMD_TOGGLE:
                if (!atomic_load(&s_session)) session_start();
                else if (s_st.state == AI_SPEAKING || atomic_load(&s_responding)) interrupt_reply();
                else session_end(NULL);
                break;
            case CMD_CONFIGURE: send_session_update(); break;
            // 一次断线会先后收到 ERROR 与 DISCONNECTED：已摘下连接就忽略后面的。
            // One drop brings both ERROR and DISCONNECTED: once the client is detached, ignore the rest.
            case CMD_WS_END: if (atomic_load(&s_session) && s_ws) ws_lost(); break;
            case CMD_WARM: warm_up(); break;
            case CMD_COOL: warm_release("page closed"); break;
            }
            const int ms = (int)((esp_timer_get_time() - t0) / 1000);
            if (ms > CTL_SLOW_MS && cmd != CMD_START) ai_log("control: %s took %d ms", k_cmd_names[cmd], ms);
            s_ctl_step = NULL;
        }
        // 到点重连。/ Reconnect when due.
        if (atomic_load(&s_session) && !s_ws && s_retry_at_us && esp_timer_get_time() >= s_retry_at_us) {
            s_retry_at_us = 0;
            ai_log("reconnecting (%d/%d)", s_retries, MAX_RETRIES);
            ctl_step("reconnect");
            if (!ws_open()) ws_lost();
            s_ctl_step = NULL;
        }
        ctl_step("stall check");
        check_rx_stall();
        s_ctl_step = NULL;
        if (atomic_load(&s_session)) mem_sample();
        // 连接还没完成、服务器也还没回过包：每秒推一次上行（见 uplink_nudge）。已经有回应就不推，2026-11-26
        // 实测在 TLS 握手中途推会占满发送缓冲，ACK 发不出去。
        // Not connected and no reply from the server yet: push the uplink each second (see uplink_nudge).
        // Once replies arrive, no push: measured 2026-11-26, a push mid-TLS-handshake filled the send
        // buffers and ACKs failed to go out.
        if (atomic_load(&s_session) && s_ws && !atomic_load(&s_ws_connected) && s_tcp_in_us < s_connect_us &&
            esp_timer_get_time() - s_connect_us > 1000000 && esp_timer_get_time() - s_nudge_us > 1000000) {
            ctl_step("uplink nudge");
            uplink_nudge();
            s_nudge_us = esp_timer_get_time();
            s_ctl_step = NULL;
        }
        // 会话结束时旧连接还在收尾：等它退出再收回 WiFi。超过 RELEASE_WAIT_MS 还不退出就放弃等待、
        // WiFi 留着（下次联网或关机时再收），宁可多耗点电也不在它还压着接收缓冲时卸载驱动。
        // The chat ended while an old connection was still shutting down: release WiFi once it exits. If it
        // has not exited after RELEASE_WAIT_MS, stop waiting and leave WiFi up (released on the next network
        // use or power-off): a little battery beats unloading the driver while it still holds receive buffers.
        if (atomic_load(&s_release_pending) && !atomic_load(&s_session)) {
            const int ms = (int)((esp_timer_get_time() - s_release_pending_us) / 1000);
            if (atomic_load(&s_reapers) == 0) {
                ai_log("old connection's task exited after %d ms; releasing network", ms);
                release_network();
                log_flush();
            } else if (ms > RELEASE_WAIT_MS) {
                atomic_store(&s_release_pending, false);
                ai_log("old connection's task still running after %d ms; WiFi left on", ms);
                log_flush();
            }
        }
        // 空闲超时：没人说话、也没在回答。/ Idle timeout: no speech and no reply.
        if (atomic_load(&s_session) && atomic_load(&s_streaming) && !atomic_load(&s_responding) && !playing_recently() &&
            esp_timer_get_time() - atomic_load(&s_last_activity_us) > (int64_t)IDLE_END_MS * 1000) {
            ctl_step("idle end");
            session_end(NULL);
            s_ctl_step = NULL;
        }
        if (atomic_load(&s_warm) && !atomic_load(&s_session) && esp_timer_get_time() >= s_warm_until_us)
            warm_release("idle for 90 s");
        if (!atomic_load(&s_session) && !atomic_load(&s_release_pending) && !atomic_load(&s_warm)) {
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
    if (xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(200)) != pdTRUE)
        ai_log("command %s dropped: queue full (control step %s)", k_cmd_names[cmd], s_ctl_step ? s_ctl_step : "-");
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

// 点下开始就显示「正在连接…」：控制任务可能还在预热（等 WiFi），开始命令要排队，不能让界面没反应。
// Show "connecting…" the moment start is tapped: the control task may still be warming up (waiting for
// WiFi) with the start command queued, and the page must not look unresponsive.
static void show_connecting(void) {
    if (!s_lock || atomic_load(&s_session) || s_st.state != AI_IDLE) return;
    atomic_store(&s_start_cancelled, false);
    lock();
    s_st.state = AI_CONNECTING;
    s_st.user[0] = s_st.reply[0] = s_st.error[0] = 0;
    unlock();
}

void ai_start(void) {
    show_connecting();
    post_and_run(CMD_START);
}
void ai_stop(void) {
    if (!ai_active()) return;
    ai_log("tap: stop (state %d, step %s)", (int)s_st.state, s_ctl_step ? s_ctl_step : "-");
    if (s_st.state == AI_CONNECTING) {
        s_cancel = true;  // 立即打断等 WiFi / interrupt the WiFi wait at once
        if (!atomic_load(&s_session)) atomic_store(&s_start_cancelled, true);
    }
    post_and_run(CMD_STOP);
}
void ai_toggle(void) {
    // 记下每次点按：卡住时能分清是界面没送到还是控制任务没处理。/ Log each tap: when stuck, this tells
    // whether the page never delivered it or the control task never handled it.
    ai_log("tap: toggle (state %d, step %s)", (int)s_st.state, s_ctl_step ? s_ctl_step : "-");
    if (s_st.state == AI_CONNECTING) {
        s_cancel = true;
        // 开始命令可能还在排队：作废它并发「结束」，别让后面的「切换」又开一次。
        // A start may still be queued: drop it and send stop, so a later toggle cannot start again.
        if (!atomic_load(&s_session)) {
            atomic_store(&s_start_cancelled, true);
            post_and_run(CMD_STOP);
            return;
        }
    } else {
        show_connecting();
    }
    post_and_run(CMD_TOGGLE);
}

void ai_page_open(bool open) {
    atomic_store(&s_page_open, open);
    if (open) {
        post_and_run(CMD_WARM);
        return;
    }
    s_cancel = true;  // 打断还在等 WiFi 的预热 / interrupt a warm-up still waiting for WiFi
    if (atomic_load(&s_warm) || s_ctl) post_and_run(CMD_COOL);
}

bool ai_warm_held(void) { return atomic_load(&s_warm); }

void ai_get_status(ai_status_t *out) {
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    lock();
    *out = s_st;
    xSemaphoreGive(s_lock);  // 只读，不改版本号 / read only: the revision stays
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

bool ai_net_busy(void) { return atomic_load(&s_reapers) > 0; }
