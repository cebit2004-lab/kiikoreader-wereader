/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：网易云在线播放，两个任务：
 *   联网任务：拉列表（GET {MUSIC_BASE}/top|likes|daily|search），以及取播放地址（/url?id=）后把 MP3 流
 *     写进 PSRAM 环形缓冲（最大 3 MB，播放时才分配）；下载的间隙也处理拉列表请求。它会启动 WiFi（读写 NVS 时缓存关闭），
 *     所以栈必须在内部 RAM；内部 RAM 紧张，按需创建，空闲 30 秒自行退出。
 *   解码任务：从缓冲取数据，minimp3 逐帧解码，立体声混单声道，必要时重采样后送 I2S（栈在 PSRAM）。
 * 每换一首歌递增 s_gen；联网任务发现代号变了就放弃旧流，解码任务只读本代的数据。
 *
 * English: NetEase online playback in two tasks:
 *   network task: loads lists (GET {MUSIC_BASE}/top|likes|daily|search) and, given /url?id=, streams
 *     the MP3 into a PSRAM ring (up to 3 MB, allocated only while playing), serving list requests between chunks. It starts WiFi, whose NVS
 *     access disables the cache, so its stack must be internal RAM; internal RAM is scarce, so it is
 *     created on demand and exits after 30 s idle.
 *   decode task: reads the ring, decodes with minimp3, mixes to mono, resamples if needed and feeds
 *     I2S (PSRAM stack).
 * Each new track bumps s_gen; the network task drops a stream whose generation is stale, and the
 * decoder reads only data of the current generation.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#include "music_player.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_voice.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "media_net.h"
#include "metalio_audio.h"
#include "psram_stream.h"
#include "read_pico_transfer.h"

#if __has_include("music_secrets.h")
#include "music_secrets.h"
#endif
#ifndef MUSIC_BASE
#define MUSIC_BASE ""
#endif
#ifndef MUSIC_TOKEN
#define MUSIC_TOKEN ""
#endif

static const char *TAG = "music";

/*
 * 环形缓冲尽量大（PSRAM 充裕）：缓冲一满下载就得停，TCP 接收窗口关闭；等腾出空间后的窗口更新要走
 * 上行，而 12 dBm 发射功率下上行易丢，服务器按 5/10/20 秒退避重探，实测每次卡 12–20 秒并导致断音。
 * 3 MB 约装 3 分钟 128k 音频，大多数歌整首下完都不必停。分不到就依次退到 1 MB、256 KB。
 * Make the ring as large as PSRAM allows: once it fills, the download must pause and the TCP receive
 * window closes; the window update after space frees goes uplink, which 12 dBm TX loses easily, and
 * the server backs off 5/10/20 s before probing — measured as 12–20 s stalls that broke playback.
 * 3 MB holds about three minutes of 128k audio, so most songs download whole without pausing. Falls
 * back to 1 MB, then 256 KB.
 */
static const size_t k_ring_sizes[] = {3 * 1024 * 1024, 1024 * 1024, 256 * 1024};
static size_t s_ring_bytes;
#define START_BYTES (24 * 1024)    // 开播前至少缓冲 / buffered before playback starts
#define IN_BUF_BYTES (16 * 1024)
#define OUT_MAX 4096
#define JSON_MAX (96 * 1024)
#define URL_MAX 512
#define AUDIO_RATE 16000

typedef enum { CMD_PLAY, CMD_TOGGLE, CMD_NEXT, CMD_PREV, CMD_STOP } cmd_type_t;
typedef struct { cmd_type_t type; int arg; SemaphoreHandle_t done; } cmd_t;
typedef enum { NET_LIST, NET_STREAM, NET_WARM } net_type_t;
typedef struct {
    net_type_t type;
    music_source_t source;  // NET_LIST
    int offset;             // NET_LIST
    uint32_t gen;           // NET_STREAM
    char text[MUSIC_QUERY_MAX];  // 搜索词或歌曲 id / query or track id
} net_req_t;

#define NET_STACK (10 * 1024)
#define NET_IDLE_MS 30000
// 音乐页开着时联网保留到最后一次操作后 90 秒（同 AI 页），接着点歌不用重新连；离开页面后照旧 30 秒空闲收回。
// While the music page is open, keep the network until 90 s after the last request (as the AI page
// does) so playing a song needs no fresh connection; after leaving, the usual 30 s idle release.
#define NET_HOLD_MS 90000
#define NET_POLL_MS 5000
// 暂停超过 10 分钟就停止播放，好让联网任务退出、收回 WiFi，设备也能自动锁屏。
// Paused for more than 10 minutes: stop, so the network task exits, WiFi is released and the device can lock.
#define PAUSE_MAX_MS (10 * 60 * 1000)
// 开播时 AI 还没放开音频（刚离开 AI 页、对话还在收尾），最多等这么久。
// How long a new track waits for the AI to let go of audio (just left the AI page, chat still ending).
#define AI_AUDIO_WAIT_MS 6000
static _Atomic bool s_page_open;
static _Atomic int64_t s_last_req_us;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmd_q;
static TaskHandle_t s_dec_task;
static TaskHandle_t s_net_task;
static portMUX_TYPE s_net_mux = portMUX_INITIALIZER_UNLOCKED;
/*
 * 联网待办只留最新的一份：列表一格、播放一格（只要最新代号）、预热一个标志，都由 s_net_mux 保护。
 * 投递不会因队列满而丢；s_wake_q 长度为 1，只用来叫醒联网任务（xQueueOverwrite 永不失败）。
 * Pending network work keeps only the latest of each: one list slot, one stream slot (latest generation
 * only) and a warm flag, all guarded by s_net_mux. Posting can never be lost to a full queue; s_wake_q has
 * length 1 and only wakes the network task (xQueueOverwrite never fails).
 */
static struct {
    bool list, warm;
    net_req_t list_req;
    uint32_t stream_gen;  // 0 为无 / 0 for none
    char stream_id[MUSIC_QUERY_MAX];
} s_pend;
static QueueHandle_t s_wake_q;
// 联网任务创建失败时，记下这一代没人去下载，解码任务据此停止而不是一直缓冲。
// When the network task could not be created, this generation has no downloader; the decoder stops
// instead of buffering forever.
static _Atomic uint32_t s_net_fail_gen;
static StreamBufferHandle_t s_ring;
static music_track_t *s_list, *s_queue;
// 状态放 PSRAM（只在任务上下文访问），初值在 music_init 里设。/ Status lives in PSRAM (task context only); music_init sets its initial values.
static EXT_RAM_BSS_ATTR music_status_t s_st;

// 下载与解码之间的代号握手。/ Generation handshake between downloader and decoder.
static _Atomic uint32_t s_gen, s_ready_gen, s_eof_gen;
static _Atomic int32_t s_content_length;
static char s_dl_error[64];  // 由下载任务在置 s_eof_gen 之前写 / written by the downloader before s_eof_gen
// 错误使换下一首也无济于事（AI 占着音频、内存不足）：直接停止。与 s_dl_error 同时写。
// The error makes skipping pointless (AI holds audio, out of memory): stop instead. Written with s_dl_error.
static _Atomic bool s_dl_fatal;

// 解码任务私有。/ Decoder-private.
static mp3dec_t *s_dec;
static uint8_t *s_in;
static int16_t *s_pcm, *s_out;
static size_t s_in_len;
static int64_t s_samples;
static int s_out_rate, s_in_rate;
static uint32_t s_phase, s_step;
static int16_t s_prev;
static bool s_resample, s_output_failed;
static int s_skips;  // 连续因无版权等原因跳过的曲目 / consecutive tracks skipped as unavailable
static int s_write_fails;  // 连续写 I2S 失败 / consecutive failed I2S writes
static int64_t s_paused_us;  // 进入暂停的时刻 / when playback was paused

// AI 正在用或预热着音频（麦克风+喇叭）：音乐不能接手或打开 I2S。
// The AI is using or holding audio warm (mic + speaker): music must not adopt or open I2S.
static bool ai_holds_audio(void) { return ai_active() || ai_audio_busy() || ai_warm_held(); }

// ---------------------------------------------------------------- status helpers

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { ++s_st.revision; xSemaphoreGive(s_lock); }
static void set_state(music_state_t state) { lock(); s_st.state = state; unlock(); }
static void set_error(const char *msg) {
    lock();
    snprintf(s_st.error, sizeof(s_st.error), "%s", msg ? msg : "");
    unlock();
}
// 列表请求仍是页面要的那份（来源、偏移，搜索时还有搜索词）。须持锁调用。
// The list request is still the one the page wants (source, offset and, for search, the query). Call locked.
static bool list_is_current(const net_req_t *req) {
    return s_st.list_source == req->source && s_st.list_offset == req->offset &&
           (req->source != MUSIC_SRC_SEARCH || strcmp(s_st.query, req->text) == 0);
}

// ---------------------------------------------------------------- network

static void url_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in && n + 4 < cap; ++in) {
        const unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("-_.~", c))) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}

// GET {MUSIC_BASE}{path}，返回 PSRAM 中的响应体（调用方释放）；失败写 err。
// GET {MUSIC_BASE}{path}; returns the body in PSRAM (caller frees) or writes err.
static char *api_get(const char *path, char *err, size_t err_size) {
    if (!MUSIC_BASE[0]) { snprintf(err, err_size, "未配置点歌服务"); return NULL; }
    char url[URL_MAX], auth[160];
    snprintf(url, sizeof(url), MUSIC_BASE "%s", path);
    snprintf(auth, sizeof(auth), "Bearer %s", MUSIC_TOKEN);
    const esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 15000, .buffer_size = 2048, .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) { snprintf(err, err_size, "内存不足"); return NULL; }
    esp_http_client_set_header(http, "Authorization", auth);
    char *body = NULL;
    if (esp_http_client_open(http, 0) != ESP_OK || esp_http_client_fetch_headers(http) < 0) {
        snprintf(err, err_size, "连接点歌服务失败");
        goto done;
    }
    const int status = esp_http_client_get_status_code(http);
    body = heap_caps_malloc(JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { snprintf(err, err_size, "内存不足"); goto done; }
    int used = 0, n;
    while (used < JSON_MAX - 1 && (n = esp_http_client_read(http, body + used, JSON_MAX - 1 - used)) > 0) used += n;
    body[used] = 0;
    if (status != 200) {
        if (status == 503) snprintf(err, err_size, "服务端网易云未登录或已过期");
        else if (status == 404) snprintf(err, err_size, "点歌服务需要更新");
        else snprintf(err, err_size, "点歌服务出错（%d）", status);
        heap_caps_free(body);
        body = NULL;
    }
done:
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    return body;
}

// 打通刚连上 WiFi 时卡住的上行（见 media_net_ensure_uplink），目标是点歌服务的主机和端口。
// Open the uplink that stalls right after joining WiFi (see media_net_ensure_uplink), towards the music
// service's host and port.
static void open_uplink(void) {
    const char *p = strstr(MUSIC_BASE, "://");
    if (!p) return;
    p += 3;
    char host[96];
    size_t n = strcspn(p, ":/");
    if (!n || n >= sizeof(host)) return;
    memcpy(host, p, n);
    host[n] = 0;
    int port = strncmp(MUSIC_BASE, "https", 5) ? 80 : 443;
    if (p[n] == ':') port = atoi(p + n + 1);
    (void)media_net_ensure_uplink(host, port, NULL, NULL);
}

// ---------------------------------------------------------------- list task

static void load_list(const net_req_t *req_in) {
    const net_req_t *const req = req_in;
    static const char *const paths[] = {"/top", "/likes", "/daily", "/search"};
    {
        char err[64] = {0}, path[MUSIC_QUERY_MAX * 3 + 64];
        if (req->source == MUSIC_SRC_SEARCH) {
            char q[MUSIC_QUERY_MAX * 3 + 1];
            url_encode(req->text, q, sizeof(q));
            snprintf(path, sizeof(path), "/search?q=%s&limit=%d", q, MUSIC_PAGE_SIZE);
        } else {
            snprintf(path, sizeof(path), "%s?limit=%d&offset=%d", paths[req->source], MUSIC_PAGE_SIZE, req->offset);
        }
        char *body = NULL;
        if (media_net_acquire(err, sizeof(err), NULL)) {
            open_uplink();
            body = api_get(path, err, sizeof(err));
        }
        cJSON *root = body ? cJSON_Parse(body) : NULL;
        if (body && !root) snprintf(err, sizeof(err), "列表格式不对");
        if (body) heap_caps_free(body);
        lock();
        // 期间又切了来源或改了搜索词就丢弃结果。/ Drop the result if the source or search query changed meanwhile.
        const bool current = list_is_current(req);
        if (current) {
            s_st.list_loading = false;
            snprintf(s_st.list_error, sizeof(s_st.list_error), "%s", err);
            s_st.list_count = 0;
            s_st.list_total = -1;
        }
        if (current && root) {
            int n = 0;
            const cJSON *item;
            cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "songs")) {
                if (n >= MUSIC_PAGE_SIZE) break;
                const cJSON *id = cJSON_GetObjectItem(item, "id");
                if (!cJSON_IsNumber(id)) continue;
                music_track_t *t = &s_list[n++];
                // id 可能超出 int 精度，按整数文本保存。/ Ids may exceed int precision; keep them as text.
                snprintf(t->id, sizeof(t->id), "%.0f", id->valuedouble);
                const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
                const char *artist = cJSON_GetStringValue(cJSON_GetObjectItem(item, "artist"));
                snprintf(t->name, sizeof(t->name), "%s", name ? name : "");
                snprintf(t->artist, sizeof(t->artist), "%s", artist ? artist : "");
            }
            const cJSON *total = cJSON_GetObjectItem(root, "total");
            s_st.list_count = n;
            s_st.list_total = cJSON_IsNumber(total) ? total->valueint : -1;
            if (!n) snprintf(s_st.list_error, sizeof(s_st.list_error), "没有歌曲");
        }
        const int count = s_st.list_count;
        unlock();
        cJSON_Delete(root);
        ESP_LOGI(TAG, "list %s -> %d songs %s", path, count, err);
    }
}

// ---------------------------------------------------------------- download task

static bool gen_current(uint32_t gen) { return atomic_load(&s_gen) == gen; }

// 结束这一代的下载。已换歌（代号过期）就什么都不写，免得盖掉新一首的状态。
// Finish this generation's download. A stale generation writes nothing, so it cannot clobber the next track.
static void dl_finish(uint32_t gen, const char *err, bool fatal) {
    if (!gen_current(gen)) return;
    snprintf(s_dl_error, sizeof(s_dl_error), "%s", err ? err : "");
    atomic_store(&s_dl_fatal, fatal);
    atomic_store(&s_eof_gen, gen);
}

// 停播后联网任务若刚替这一代打开了 I2S，就关掉；音乐又在播或 AI 在用时不碰。
// If the network task just opened I2S for a generation that was then stopped, close it; leave it alone
// while music plays again or the AI uses it.
static void close_if_abandoned(void) {
    if (!music_active() && !ai_holds_audio() && metalio_audio_is_open()) {
        ESP_LOGI(TAG, "closing I2S opened for a stopped track");
        metalio_audio_close();
    }
}

// 等 AI 放开音频（对话收尾、预热收回），最多 AI_AUDIO_WAIT_MS；期间换歌就放弃。返回 AI 是否已放开。
// Wait up to AI_AUDIO_WAIT_MS for the AI to let go of audio (chat ending, warm-up released); give up if
// the track changes. Returns whether the AI let go.
static bool wait_ai_audio_free(uint32_t gen) {
    for (int waited = 0; ai_holds_audio() && gen_current(gen) && waited < AI_AUDIO_WAIT_MS; waited += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    return !ai_holds_audio();
}

// 取出待办。list_only 时只取列表（下载间隙用）。/ Take pending work; list_only takes lists only (between chunks).
static bool take_job(net_req_t *out, bool list_only) {
    bool got = false;
    taskENTER_CRITICAL(&s_net_mux);
    if (!list_only && s_pend.stream_gen) {
        out->type = NET_STREAM;
        out->gen = s_pend.stream_gen;
        memcpy(out->text, s_pend.stream_id, sizeof(out->text));
        s_pend.stream_gen = 0;
        s_pend.warm = false;  // 播放本身就会联网 / streaming joins the network anyway
        got = true;
    } else if (s_pend.list) {
        *out = s_pend.list_req;
        s_pend.list = false;
        if (!list_only) s_pend.warm = false;
        got = true;
    } else if (!list_only && s_pend.warm) {
        out->type = NET_WARM;
        s_pend.warm = false;
        got = true;
    }
    taskEXIT_CRITICAL(&s_net_mux);
    return got;
}

#define STREAM_RETRIES 5

static void stream_url(uint32_t gen, const char *url) {
    static uint8_t *chunk;
    if (!chunk) chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) { dl_finish(gen, "内存不足", true); return; }
    const char *err = NULL;
    int64_t got = 0, length = 0;
    // 中途卡死或断开时用 Range 从已下载处续传，而不是结束这首歌（12 dBm 上行偶尔会让连接卡住几十秒）。
    // 每次续传有进展就重新计数，只有连续 STREAM_RETRIES 次毫无进展才放弃。
    // On a mid-stream stall or drop, resume with Range from the bytes already fetched instead of ending
    // the track (weak 12 dBm uplink occasionally freezes a connection for tens of seconds). The count
    // restarts whenever a resume made progress; only STREAM_RETRIES attempts in a row without progress give up.
    for (int attempt = 0; attempt <= STREAM_RETRIES && gen_current(gen); ++attempt) {
        if (attempt) {
            ESP_LOGW(TAG, "stream stalled at %lld/%lld bytes, resuming (%d/%d)", (long long)got, (long long)length,
                     attempt, STREAM_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        const int64_t got_before = got;
        const esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = 10000, .buffer_size = 4096, .buffer_size_tx = 1024,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        if (!http) { err = "内存不足"; break; }
        if (got > 0) {
            char range[48];
            snprintf(range, sizeof(range), "bytes=%lld-", (long long)got);
            esp_http_client_set_header(http, "Range", range);
        }
        bool retry = false;
        err = NULL;
        if (esp_http_client_open(http, 0) != ESP_OK) {
            err = "歌曲连接失败";
            retry = true;
        } else {
            const int64_t declared = esp_http_client_fetch_headers(http);
            const int status = esp_http_client_get_status_code(http);
            if (got > 0 && status != 206) {
                err = "无法续传";  // 服务器不支持 Range：保留已下载部分 / no Range support: keep what we have
            } else if (status != 200 && status != 206) {
                err = "歌曲下载失败";
            } else {
                if (got == 0) {
                    length = declared > 0 ? declared : 0;
                    atomic_store(&s_content_length, length > 0 && length < INT32_MAX ? (int32_t)length : 0);
                    ESP_LOGI(TAG, "stream %s, %lld bytes", strncmp(url, "https:", 6) ? "http" : "https", (long long)length);
                }
                // TEMP 下载速度。/ TEMP download throughput.
                const int64_t t0 = esp_timer_get_time(), base = got;
                int64_t next_log = got + 64 * 1024;
                for (;;) {
                    const int n = esp_http_client_read(http, (char *)chunk, 4096);
                    if (n < 0) { err = "网络中断"; retry = true; break; }
                    if (n == 0) {
                        // 声明了长度却没收满：连接被提前关掉，续传。/ Short of the declared length: closed early, resume.
                        if (length > 0 && got < length) { err = "网络中断"; retry = true; }
                        break;
                    }
                    got += n;
                    if (got >= next_log) {
                        const int64_t ms = (esp_timer_get_time() - t0) / 1000;
                        ESP_LOGI(TAG, "download %lld KB, %lld KB/s, ring %u%%", (long long)(got / 1024),
                                 ms > 0 ? (long long)((got - base) * 1000 / 1024 / ms) : 0LL,
                                 (unsigned)(xStreamBufferBytesAvailable(s_ring) * 100 / s_ring_bytes));
                        next_log += 64 * 1024;
                    }
                    // 软件解密很吃 CPU：每块让出一下。/ Software TLS decryption is CPU-heavy: yield per chunk.
                    vTaskDelay(1);
                    size_t sent = 0;
                    while (sent < (size_t)n) {
                        // 已换歌或已停止（含暂停超时）就放手。/ Let go once the track changed or stopped (pause timeout included).
                        if (!gen_current(gen)) break;
                        // 下载间隙处理拉列表；换歌请求留给主循环。/ Serve list loads between chunks; leave track changes to the loop.
                        net_req_t req;
                        if (take_job(&req, true)) load_list(&req);
                        sent += xStreamBufferSend(s_ring, chunk + sent, (size_t)n - sent, pdMS_TO_TICKS(100));
                    }
                    if (!gen_current(gen)) break;
                }
            }
        }
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        if (!retry) break;
        if (got > got_before) attempt = 0;  // 有进展：重新计数 / progress made: count again
    }
    dl_finish(gen, err, false);
}

static void stream_track(const net_req_t *req) {
    const uint32_t gen = req->gen;
    if (!gen_current(gen)) return;
    // 清掉上一首的残余；解码任务正阻塞读取时复位会失败，稍后重试。
    // Drop the previous track's leftovers; reset fails while the decoder blocks on a read, so retry.
    while (xStreamBufferReset(s_ring) != pdPASS && gen_current(gen)) vTaskDelay(pdMS_TO_TICKS(5));
    if (!gen_current(gen)) return;
    atomic_store(&s_content_length, 0);
    atomic_store(&s_ready_gen, gen);
    char err[64] = {0}, path[MUSIC_QUERY_MAX + 16];
    if (!media_net_acquire(err, sizeof(err), NULL)) { dl_finish(gen, err, false); return; }
    // 连 WiFi 可能要几十秒：期间停了或换了歌就不再开 I2S。/ Joining WiFi can take tens of seconds: no I2S if the track was stopped or changed meanwhile.
    if (!gen_current(gen)) return;
    // AI 占着音频时不接手也不打开 I2S，等它放开；等不到就停止（换下一首也一样）。
    // While the AI holds audio, neither adopt nor open I2S: wait for it to let go, else stop (skipping would not help).
    if (!wait_ai_audio_free(gen)) { dl_finish(gen, "AI 助手正在使用音频", true); return; }
    if (!gen_current(gen)) return;
    // I2S 的 DMA 缓冲只能在内部 RAM。WiFi 要约 28 KB，先开 I2S 会让 WiFi 起不来；而 WiFi 刚连上、
    // 还没开始下载时，碎片里仍放得下 I2S 的几小块缓冲。所以连上网就立刻占住 I2S，再开始下载。
    // 万一仍分不到，就断网、先开 I2S、再重连。
    // I2S DMA buffers must be internal RAM. WiFi needs ~28 KB, so opening I2S first starves WiFi; right
    // after WiFi connects, before any download, the small I2S buffers still fit. Claim I2S then, before
    // streaming; failing that, drop WiFi, open I2S first and reconnect.
    if (!metalio_audio_is_open() && metalio_audio_open(AUDIO_RATE, false) != ESP_OK) {
        ESP_LOGW(TAG, "I2S did not fit beside WiFi; reconnecting after claiming it");
        media_net_drop();
        if (metalio_audio_open(AUDIO_RATE, false) != ESP_OK) { dl_finish(gen, "音频设备不可用", true); return; }
        if (!media_net_acquire(err, sizeof(err), NULL)) { dl_finish(gen, err, false); return; }
    }
    if (!gen_current(gen)) { close_if_abandoned(); return; }
    open_uplink();  // 预热过就立即返回 / returns at once after a warm-up
    if (!gen_current(gen)) { close_if_abandoned(); return; }
    snprintf(path, sizeof(path), "/url?id=%s", req->text);
    char *body = api_get(path, err, sizeof(err));
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    if (body) heap_caps_free(body);
    const char *url = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "url")) : NULL;
    if (!gen_current(gen)) {
        close_if_abandoned();
    } else if (!url) {
        dl_finish(gen, err[0] ? err : "无版权或需要会员", false);
    } else {
        if (cJSON_IsTrue(cJSON_GetObjectItem(root, "trial"))) ESP_LOGW(TAG, "track %s is a trial clip", req->text);
        ESP_LOGI(TAG, "stream %s", req->text);
        stream_url(gen, url);
    }
    cJSON_Delete(root);
}

static bool net_pending_locked(void) { return s_pend.list || s_pend.warm || s_pend.stream_gen; }

static void net_task(void *arg) {
    (void)arg;
    net_req_t req;
    for (;;) {
        if (!take_job(&req, false)) {
            uint8_t token;
            if (xQueueReceive(s_wake_q, &token, pdMS_TO_TICKS(NET_POLL_MS)) == pdTRUE) continue;
            // 空闲：没在播放（暂停不算）、也过了保留时间就收回网络并退出，把内部 RAM 还回去。
            // 暂停时缓冲里还有歌，留着缓冲，只收网络。
            // Idle: when not playing (paused does not count) and past the hold, release the network and exit
            // to return the internal RAM. While paused the ring still holds the song: keep it, drop only the network.
            if (music_playing()) continue;
            const int64_t idle_ms = (esp_timer_get_time() - atomic_load(&s_last_req_us)) / 1000;
            if (idle_ms < (atomic_load(&s_page_open) ? NET_HOLD_MS : NET_IDLE_MS)) continue;
            StreamBufferHandle_t ring = NULL;
            taskENTER_CRITICAL(&s_net_mux);
            const bool quit = !net_pending_locked() && !music_playing();
            if (quit) {
                s_net_task = NULL;
                if (!music_active()) { ring = s_ring; s_ring = NULL; }
            }
            taskEXIT_CRITICAL(&s_net_mux);
            if (!quit) continue;
            // 解码任务已停，不再读它；不用 IDF 6.1 有缺陷的 WithCaps 删除（见 psram_stream.h）。
            // The decoder has stopped reading it; avoids the faulty IDF 6.1 WithCaps delete (see psram_stream.h).
            if (ring) psram_stream_delete(ring);
            media_net_release_if_unused();
            ESP_LOGI(TAG, "network task exits");
            vTaskDeleteWithCaps(NULL);
        }
        if (req.type == NET_WARM) {
            // 打开音乐页：先连上 WiFi 并打通上行，点歌时直接开始。/ Music page opened: join WiFi and open the uplink now, so a song starts at once.
            char err[64] = {0};
            const int64_t t0 = esp_timer_get_time();
            if (media_net_acquire(err, sizeof(err), NULL)) {
                open_uplink();
                ESP_LOGI(TAG, "warm: network ready in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
            } else {
                ESP_LOGW(TAG, "warm: %s", err);
            }
        } else if (req.type == NET_LIST) {
            load_list(&req);
            // 不在这里收网：联网任务在期间算作占用，空闲退出时再收（见 music_net_busy）。
            // No release here: the network counts as in use while this task lives; it is released when the task exits idle.
        } else {
            stream_track(&req);
        }
    }
}

// 联网任务没能创建：清掉待办，列表报错、这一代播放交给解码任务停止，不让“加载中”“缓冲中”卡住。
// The network task could not be created: clear the pending work, fail the list, and let the decoder stop
// this generation, so neither "loading" nor "buffering" gets stuck.
static void net_start_failed(void) {
    net_req_t list = {0};
    taskENTER_CRITICAL(&s_net_mux);
    s_net_task = NULL;
    const bool had_list = s_pend.list;
    if (had_list) list = s_pend.list_req;
    const uint32_t gen = s_pend.stream_gen;
    s_pend.list = s_pend.warm = false;
    s_pend.stream_gen = 0;
    taskEXIT_CRITICAL(&s_net_mux);
    if (gen) atomic_store(&s_net_fail_gen, gen);
    if (had_list) {
        lock();
        if (list_is_current(&list)) {
            s_st.list_loading = false;
            snprintf(s_st.list_error, sizeof(s_st.list_error), "内存不足，请稍后再试");
        }
        unlock();
    }
}

// 投递联网请求，必要时以内部 RAM 栈创建联网任务；创建失败返回 false（已按上面处理）。
// Post a network request, creating the task with an internal stack if needed; false when creation failed
// (handled as above).
static bool net_post(const net_req_t *req) {
    atomic_store(&s_last_req_us, esp_timer_get_time());
    taskENTER_CRITICAL(&s_net_mux);
    if (req->type == NET_LIST) {
        s_pend.list_req = *req;
        s_pend.list = true;
    } else if (req->type == NET_STREAM) {
        s_pend.stream_gen = req->gen;
        memcpy(s_pend.stream_id, req->text, sizeof(s_pend.stream_id));
    } else {
        s_pend.warm = true;
    }
    const bool need = s_net_task == NULL;
    if (need) s_net_task = (TaskHandle_t)1;  // 占位，防止并发重复创建 / placeholder against double creation
    taskEXIT_CRITICAL(&s_net_mux);
    const uint8_t token = 0;
    xQueueOverwrite(s_wake_q, &token);
    if (!need) return true;
    TaskHandle_t task = NULL;
    // 栈在主内部 RAM：要启动 WiFi（关缓存）所以不能放 PSRAM；也不放 RTC 内存，它太慢，
    // 软件 TLS 握手会拖到 5 秒以上。按需创建、空闲退出，不常驻。
    // Stack in main internal RAM: it starts WiFi (cache off), so not PSRAM; and not RTC memory, whose
    // slow bus stretches a software TLS handshake past 5 s. Created on demand, exits when idle.
    ESP_LOGI(TAG, "net task: internal free=%u largest=%u, dma free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    // 优先级与核心同微信读书任务（2，不绑核）。/ Priority and affinity match the WeRead worker (2, unpinned).
    if (xTaskCreatePinnedToCoreWithCaps(net_task, "music_net", NET_STACK, NULL, 2, &task, tskNO_AFFINITY,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "network task: no internal memory");
        net_start_failed();
        return false;
    }
    taskENTER_CRITICAL(&s_net_mux);
    if (s_net_task == (TaskHandle_t)1) s_net_task = task;
    taskEXIT_CRITICAL(&s_net_mux);
    return true;
}

// ---------------------------------------------------------------- decoder

static bool prepare_output(int hz) {
    if (s_out_rate && s_in_rate == hz) return true;
    // BH1098G 时钟固定 16 kHz，所有文件都重采样到它。I2S 通常已由联网任务在开 WiFi 前打开。
    // The BH1098G clock is fixed at 16 kHz and every file is resampled to it. The network task
    // normally opened I2S before starting WiFi.
    // AI 占着音频时不打开、也不接手它的 I2S（它随时会关掉）。/ Never open or adopt the AI's I2S (it may close it any time).
    if (ai_holds_audio()) {
        set_error("AI 助手正在使用音频");
        s_output_failed = true;
        return false;
    }
    if (metalio_audio_open(AUDIO_RATE, false) != ESP_OK) {
        set_error("音频设备不可用");
        s_output_failed = true;
        return false;
    }
    const int opened = metalio_audio_sample_rate();
    int actual = metalio_audio_actual_rate();
    if (actual <= 0) actual = opened;
    s_resample = abs(actual - hz) * 100 > hz * 3;
    s_out_rate = s_resample ? actual : hz;
    if (s_resample) {
        static const int std_rates[] = {8000, 16000, 22050, 32000, 44100, 48000};
        for (size_t i = 0; i < sizeof(std_rates) / sizeof(std_rates[0]); ++i)
            if (abs(actual - std_rates[i]) * 100 < std_rates[i] * 3) s_out_rate = std_rates[i];
    }
    s_in_rate = hz;
    s_step = (uint32_t)(((uint64_t)hz << 16) / (uint64_t)s_out_rate);
    s_phase = 0;
    s_prev = 0;
    ESP_LOGI(TAG, "output file=%d Hz bh=%d Hz resample=%d", hz, s_out_rate, s_resample);
    return true;
}

// 线性插值重采样；s_prev 跨帧衔接。/ Linear-interpolation resampling; s_prev bridges frames.
static int resample(const int16_t *in, int n, int16_t *out) {
    int produced = 0;
    while (produced < OUT_MAX) {
        const uint32_t idx = s_phase >> 16;
        if ((int)idx >= n) break;
        const int32_t a = idx == 0 ? s_prev : in[idx - 1];
        const int32_t b = in[idx];
        out[produced++] = (int16_t)(a + (((b - a) * (int32_t)(s_phase & 0xffff)) >> 16));
        s_phase += s_step;
    }
    s_phase -= (uint32_t)n << 16;
    s_prev = in[n - 1];
    return produced;
}

static bool stream_done(void) {
    return atomic_load(&s_eof_gen) == atomic_load(&s_gen) && xStreamBufferIsEmpty(s_ring);
}

typedef enum { FRAME_OK, FRAME_WAIT, FRAME_END, FRAME_FAIL } frame_result_t;

static frame_result_t play_frame(void) {
    const uint32_t gen = atomic_load(&s_gen);
    if (atomic_load(&s_ready_gen) != gen) {
        // 联网任务没建起来，这一代没人下载：停止而不是一直缓冲。/ No network task for this generation: stop instead of buffering forever.
        if (atomic_load(&s_net_fail_gen) == gen) { set_error("内存不足，无法联网"); return FRAME_FAIL; }
        vTaskDelay(pdMS_TO_TICKS(20));
        return FRAME_WAIT;
    }
    const bool eof = atomic_load(&s_eof_gen) == gen;
    // 开播与欠载恢复的缓冲门限。/ Buffer gate for start-up and underrun recovery.
    if (s_st.state == MUSIC_BUFFERING) {
        if (xStreamBufferBytesAvailable(s_ring) < START_BYTES && !eof) { vTaskDelay(pdMS_TO_TICKS(50)); return FRAME_WAIT; }
        set_state(MUSIC_PLAYING);
    }
    if (s_in_len < IN_BUF_BYTES / 2)
        s_in_len += xStreamBufferReceive(s_ring, s_in + s_in_len, IN_BUF_BYTES - s_in_len, pdMS_TO_TICKS(20));
    if (s_in_len == 0) {
        if (stream_done()) return FRAME_END;
        if (!eof && s_samples > 0) { set_state(MUSIC_BUFFERING); ESP_LOGW(TAG, "underrun"); }
        return FRAME_WAIT;
    }
    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame(s_dec, s_in, (int)s_in_len, s_pcm, &info);
    if (info.frame_bytes == 0) {
        // 缓冲里还没有完整帧：再等数据，或已到结尾。/ No whole frame yet: wait for data, or the end.
        if (stream_done() || s_in_len >= IN_BUF_BYTES) return FRAME_END;
        s_in_len += xStreamBufferReceive(s_ring, s_in + s_in_len, IN_BUF_BYTES - s_in_len, pdMS_TO_TICKS(20));
        return FRAME_WAIT;
    }
    memmove(s_in, s_in + info.frame_bytes, s_in_len - (size_t)info.frame_bytes);
    s_in_len -= (size_t)info.frame_bytes;
    if (samples <= 0) return FRAME_OK;
    if (!prepare_output(info.hz)) return FRAME_FAIL;
    if (info.channels == 2)
        for (int i = 0; i < samples; ++i) s_pcm[i] = (int16_t)(((int32_t)s_pcm[2 * i] + s_pcm[2 * i + 1]) / 2);
    const int16_t *out = s_pcm;
    int n = samples;
    if (s_resample) { n = resample(s_pcm, samples, s_out); out = s_out; }
    if (metalio_audio_write(out, (size_t)n, pdMS_TO_TICKS(500)) < 0) {
        // 输出通道没了（被别处关掉）：丢掉这一帧，下一帧重新打开；连续失败就停止。
        // The output channel is gone (closed elsewhere): drop this frame and reopen on the next; stop if it keeps failing.
        s_out_rate = 0;
        if (++s_write_fails >= 3) { set_error("音频设备不可用"); return FRAME_FAIL; }
        return FRAME_WAIT;
    }
    s_write_fails = 0;
    s_samples += samples;
    s_skips = 0;

    const int pos = (int)(s_samples / info.hz);
    const int32_t length = atomic_load(&s_content_length);
    if (pos != s_st.position_s || (s_st.duration_s == 0 && length > 0 && info.bitrate_kbps > 0)) {
        lock();
        s_st.position_s = pos;
        if (s_st.duration_s == 0 && length > 0 && info.bitrate_kbps > 0)
            s_st.duration_s = (int)((int64_t)length * 8 / 1000 / info.bitrate_kbps);
        s_st.buffered_pct = (int)(xStreamBufferBytesAvailable(s_ring) * 100 / s_ring_bytes);
        unlock();
    }
    return FRAME_OK;
}

static int pick_next(int current, int dir) {
    const int count = s_st.queue_count;
    if (count <= 0) return -1;
    if (s_st.mode == MUSIC_SHUFFLE && count > 1) {
        int r;
        do r = (int)(esp_random() % (uint32_t)count); while (r == current);
        return r;
    }
    if (current < 0) return 0;
    return (current + dir + count) % count;
}

// 播放时才分配环形缓冲；停止后由空闲退出的联网任务或 stop_playback 释放。
// Allocate the ring only for playback; once stopped, the idle network task or stop_playback frees it.
static bool ensure_ring(void) {
    for (size_t i = 0; !s_ring && i < sizeof(k_ring_sizes) / sizeof(k_ring_sizes[0]); ++i) {
        s_ring = psram_stream_create(k_ring_sizes[i], 1);
        if (s_ring) {
            s_ring_bytes = k_ring_sizes[i];
            ESP_LOGI(TAG, "ring buffer %u KB", (unsigned)(s_ring_bytes / 1024));
        }
    }
    return s_ring != NULL;
}

static void start_track(int index) {
    if (index < 0 || index >= s_st.queue_count) return;
    const uint32_t gen = atomic_fetch_add(&s_gen, 1) + 1;
    mp3dec_init(s_dec);
    s_in_len = 0;
    s_samples = 0;
    net_req_t req = {.type = NET_STREAM, .gen = gen};
    lock();
    s_st.queue_index = index;
    s_st.state = MUSIC_BUFFERING;
    s_st.position_s = 0;
    s_st.duration_s = 0;
    s_st.buffered_pct = 0;
    s_st.error[0] = 0;
    snprintf(s_st.title, sizeof(s_st.title), "%s", s_queue[index].name);
    snprintf(s_st.artist, sizeof(s_st.artist), "%s", s_queue[index].artist);
    snprintf(req.text, sizeof(req.text), "%s", s_queue[index].id);
    unlock();
    // 先置为缓冲中再分配：联网任务看到正在播放，就不会在此刻释放缓冲。
    // Mark buffering before allocating, so the network task cannot free the ring meanwhile.
    if (!ensure_ring()) {
        set_error("内存不足");
        set_state(MUSIC_STOPPED);
        return;
    }
    // 投递失败（联网任务建不起来）时 play_frame 看到 s_net_fail_gen 就停止。/ If posting fails, play_frame sees s_net_fail_gen and stops.
    (void)net_post(&req);
}

static void stop_playback(void) {
    atomic_fetch_add(&s_gen, 1);  // 下载任务放弃当前流 / the downloader abandons its stream
    // AI 正在用或马上要用音频（开始对话时先停音乐）时不碰 I2S 和功放：它会自己按需重开。
    // Leave I2S and the amplifier alone while the AI uses or is about to use audio (starting a chat stops
    // music first): it reopens I2S as it needs.
    if (!ai_holds_audio()) metalio_audio_close();
    s_out_rate = 0;
    s_write_fails = 0;
    set_state(MUSIC_STOPPED);
    // 联网任务已退出（如暂停期间退出）就由这里释放环形缓冲；还在的话它空闲退出时释放。
    // If the network task has already exited (say, during a pause), free the ring here; otherwise it does
    // so when it exits idle.
    StreamBufferHandle_t ring = NULL;
    taskENTER_CRITICAL(&s_net_mux);
    if (s_net_task == NULL) { ring = s_ring; s_ring = NULL; }
    taskEXIT_CRITICAL(&s_net_mux);
    if (ring) psram_stream_delete(ring);
    // WiFi 由联网任务空闲退出时收回（关 WiFi 要碰 NVS，不能在 PSRAM 栈上做）。
    // The network task releases WiFi when it goes idle (stopping WiFi touches NVS; not on a PSRAM stack).
}

static void handle(const cmd_t *cmd) {
    const int cur = s_st.queue_index;
    switch (cmd->type) {
    case CMD_PLAY:
        lock();
        // 当前列表成为播放队列。/ The current list becomes the play queue.
        memcpy(s_queue, s_list, sizeof(music_track_t) * (size_t)s_st.list_count);
        s_st.queue_count = s_st.list_count;
        s_st.queue_source = s_st.list_source;
        s_st.queue_offset = s_st.list_offset;
        unlock();
        s_skips = 0;
        start_track(cmd->arg);
        break;
    case CMD_TOGGLE:
        if (s_st.state == MUSIC_PLAYING || s_st.state == MUSIC_BUFFERING) {
            s_paused_us = esp_timer_get_time();
            set_state(MUSIC_PAUSED);
            metalio_audio_set_pa(false);
        } else if (s_st.state == MUSIC_PAUSED) {
            set_state(s_samples > 0 ? MUSIC_PLAYING : MUSIC_BUFFERING);
        } else if (s_st.queue_count > 0) {
            start_track(cur >= 0 ? cur : 0);
        }
        break;
    case CMD_NEXT: start_track(pick_next(cur, 1)); break;
    case CMD_PREV: start_track(s_st.position_s > 3 && cur >= 0 ? cur : pick_next(cur, -1)); break;
    case CMD_STOP: stop_playback(); break;
    }
    if (cmd->done) xSemaphoreGive(cmd->done);
}

static void decode_task(void *arg) {
    (void)arg;
    cmd_t cmd;
    for (;;) {
        // 只有解码任务改 state，这里取一次快照。/ Only this task changes state; take one snapshot.
        const music_state_t state = s_st.state;
        TickType_t wait = portMAX_DELAY;
        if (state == MUSIC_PLAYING || state == MUSIC_BUFFERING) {
            wait = 0;
        } else if (state == MUSIC_PAUSED) {
            const int64_t left_ms = PAUSE_MAX_MS - (esp_timer_get_time() - s_paused_us) / 1000;
            wait = left_ms > 0 ? pdMS_TO_TICKS(left_ms) : 0;
        }
        if (xQueueReceive(s_cmd_q, &cmd, wait) == pdTRUE) {
            handle(&cmd);
            continue;
        }
        if (state == MUSIC_PAUSED) {
            // 暂停太久：停止，下载随之放手，联网任务空闲后退出并收回 WiFi。
            // Paused too long: stop; the download lets go and the network task exits idle, releasing WiFi.
            ESP_LOGI(TAG, "paused for %d min; stopping", PAUSE_MAX_MS / 60000);
            stop_playback();
            set_error("暂停过久，已停止");
            continue;
        }
        if (state == MUSIC_STOPPED) continue;
        const frame_result_t r = play_frame();
        if (r == FRAME_FAIL || s_output_failed) { s_output_failed = false; stop_playback(); continue; }
        if (r != FRAME_END) continue;
        // 曲目结束。下载出错（无版权、网络）或一个样本都没解出来都算失败，自动跳过；整轮都失败、
        // 没联网或换歌也无济于事时停止。没解出样本也算失败，单曲循环与列表循环才不会反复下载同一首。
        // End of track. A download error (unavailable, network) or not one decoded sample is a failure and
        // is skipped; stop after a whole failed round, without WiFi, or when skipping cannot help. Counting
        // zero samples as failure keeps repeat-one and repeat-all from re-downloading the same track forever.
        const bool failed = s_samples == 0;
        if (failed) {
            const bool eof = atomic_load(&s_eof_gen) == atomic_load(&s_gen);
            const char *why = eof && s_dl_error[0] ? s_dl_error : "无法解码";
            set_error(why);
            ESP_LOGW(TAG, "track %d failed: %s", s_st.queue_index, why);
            const bool fatal = eof && (atomic_load(&s_dl_fatal) || strstr(why, "WiFi") || strstr(why, "服务"));
            if (++s_skips >= s_st.queue_count || fatal) {
                stop_playback();
                continue;
            }
        }
        start_track(s_st.mode == MUSIC_REPEAT_ONE && !failed ? s_st.queue_index : pick_next(s_st.queue_index, 1));
    }
}

// ---------------------------------------------------------------- public API

// wait 时一直等到解码任务处理完：超时后删掉信号量而命令仍在队列里，解码任务之后就会去 give 一个已删除的信号量，
// 调用方（开始 AI 对话、进入 U 盘模式）也会误以为音乐已停。解码任务处理命令不会无限阻塞。
// With wait, block until the decoder has handled it: deleting the semaphore after a timeout while the
// command is still queued would have the decoder give a freed object, and the caller (starting an AI
// chat, entering USB drive mode) would wrongly assume music had stopped. The decoder never blocks
// indefinitely while handling a command.
static void post(cmd_type_t type, int arg, bool wait) {
    if (!s_cmd_q || !s_dec_task) return;
    cmd_t cmd = {.type = type, .arg = arg, .done = wait ? xSemaphoreCreateBinary() : NULL};
    if (xQueueSend(s_cmd_q, &cmd, wait ? portMAX_DELAY : pdMS_TO_TICKS(200)) == pdTRUE && cmd.done)
        xSemaphoreTake(cmd.done, portMAX_DELAY);
    if (cmd.done) vSemaphoreDelete(cmd.done);
}

esp_err_t music_init(void) {
    if (s_cmd_q) return ESP_OK;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(8, sizeof(cmd_t));
    s_wake_q = xQueueCreate(1, sizeof(uint8_t));
    s_list = heap_caps_calloc(MUSIC_PAGE_SIZE, sizeof(music_track_t), caps);
    s_queue = heap_caps_calloc(MUSIC_PAGE_SIZE, sizeof(music_track_t), caps);
    s_dec = heap_caps_malloc(sizeof(mp3dec_t), caps);
    s_in = heap_caps_malloc(IN_BUF_BYTES, caps);
    s_pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), caps);
    s_out = heap_caps_malloc(OUT_MAX * sizeof(int16_t), caps);
    if (!s_lock || !s_cmd_q || !s_wake_q || !s_list || !s_queue ||
        !s_dec || !s_in || !s_pcm || !s_out)
        return ESP_ERR_NO_MEM;
    s_st.state = MUSIC_STOPPED;
    s_st.queue_index = -1;
    s_st.list_total = -1;
    s_st.volume = metalio_audio_volume();
    // minimp3 栈上约 16 KB；解码任务不碰 flash，栈放 PSRAM。联网任务按需创建。
    // minimp3 needs ~16 KB of stack; the decoder never touches flash, so its stack lives in PSRAM.
    // The network task is created on demand.
    if (xTaskCreatePinnedToCoreWithCaps(decode_task, "music_dec", 24 * 1024, NULL, 6, &s_dec_task, 1, caps) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void music_load(music_source_t source, const char *query, int offset) {
    if (!s_wake_q || source >= MUSIC_SRC_COUNT) return;
    net_req_t req = {.type = NET_LIST, .source = source, .offset = offset < 0 ? 0 : offset};
    snprintf(req.text, sizeof(req.text), "%s", query ? query : "");
    // 空搜索词不请求服务器（它会回 400）：只切到搜索，列表留空等输入。
    // An empty search query does not hit the server (it answers 400): just switch to search with an empty list.
    const bool empty_search = source == MUSIC_SRC_SEARCH && !req.text[0];
    lock();
    s_st.list_source = source;
    s_st.list_offset = req.offset;
    s_st.list_loading = !empty_search;
    s_st.list_error[0] = 0;
    snprintf(s_st.query, sizeof(s_st.query), "%s", req.text);
    if (empty_search) {
        s_st.list_count = 0;
        s_st.list_total = -1;
    }
    unlock();
    // 投递失败时 net_post 已清掉“加载中”并写好错误。/ On failure net_post has already cleared loading and set the error.
    if (!empty_search) (void)net_post(&req);
}

void music_page_open(bool open) {
    atomic_store(&s_page_open, open);
    if (!open || !s_wake_q) return;
    if (!music_playing()) {
        const net_req_t req = {.type = NET_WARM};
        net_post(&req);
    } else {
        atomic_store(&s_last_req_us, esp_timer_get_time());
    }
}

bool music_list_track(int index, music_track_t *out) {
    if (!s_lock || !out) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = index >= 0 && index < s_st.list_count;
    if (ok) *out = s_list[index];
    xSemaphoreGive(s_lock);
    return ok;
}

void music_play(int index) { post(CMD_PLAY, index, false); }
void music_toggle(void) { post(CMD_TOGGLE, 0, false); }
void music_next(void) { post(CMD_NEXT, 0, false); }
void music_prev(void) { post(CMD_PREV, 0, false); }
void music_stop(void) { post(CMD_STOP, 0, true); }

void music_set_volume(int volume) {
    metalio_audio_set_volume(volume);
    if (!s_lock) return;
    lock();
    s_st.volume = metalio_audio_volume();
    unlock();
}

void music_cycle_mode(void) {
    if (!s_lock) return;
    lock();
    s_st.mode = (music_mode_t)((s_st.mode + 1) % 3);
    unlock();
}

void music_get_status(music_status_t *out) {
    if (!out) return;
    if (!s_lock) { *out = s_st; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

bool music_active(void) { return s_st.state != MUSIC_STOPPED; }
bool music_playing(void) { return s_st.state == MUSIC_PLAYING || s_st.state == MUSIC_BUFFERING; }
bool music_net_busy(void) {
    // 联网任务还在（含音乐页开着时的保留期）也算占用，免得 AI 结束时把预热好的 WiFi 收掉。暂停本身不占网络：
    // 暂停时联网任务空闲退出就收回 WiFi，缓冲里的歌留着。
    // The network task being alive (including the hold while the music page is open) counts as use, so an
    // AI chat ending does not drop the warmed-up WiFi. A pause alone does not hold the network: once the
    // network task exits idle during a pause WiFi is released, and the buffered song stays.
    return music_playing() || s_st.list_loading || s_net_task != NULL;
}
