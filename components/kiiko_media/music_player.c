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
static _Atomic bool s_page_open;
static _Atomic int64_t s_last_req_us;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmd_q, s_net_q;
static TaskHandle_t s_net_task;
static portMUX_TYPE s_net_mux = portMUX_INITIALIZER_UNLOCKED;
static StreamBufferHandle_t s_ring;
static music_track_t *s_list, *s_queue;
// 状态放 PSRAM（只在任务上下文访问），初值在 music_init 里设。/ Status lives in PSRAM (task context only); music_init sets its initial values.
static EXT_RAM_BSS_ATTR music_status_t s_st;

// 下载与解码之间的代号握手。/ Generation handshake between downloader and decoder.
static _Atomic uint32_t s_gen, s_ready_gen, s_eof_gen;
static _Atomic int32_t s_content_length;
static char s_dl_error[64];  // 由下载任务在置 s_eof_gen 之前写 / written by the downloader before s_eof_gen

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

// ---------------------------------------------------------------- status helpers

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { ++s_st.revision; xSemaphoreGive(s_lock); }
static void set_state(music_state_t state) { lock(); s_st.state = state; unlock(); }
static void set_error(const char *msg) {
    lock();
    snprintf(s_st.error, sizeof(s_st.error), "%s", msg ? msg : "");
    unlock();
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
        // 期间又切了来源就丢弃结果。/ Drop the result if the source changed meanwhile.
        const bool current = s_st.list_source == req->source && s_st.list_offset == req->offset;
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

static void dl_finish(uint32_t gen, const char *err) {
    snprintf(s_dl_error, sizeof(s_dl_error), "%s", err ? err : "");
    atomic_store(&s_eof_gen, gen);
}

#define STREAM_RETRIES 5

static void stream_url(uint32_t gen, const char *url) {
    static uint8_t *chunk;
    if (!chunk) chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) { dl_finish(gen, "内存不足"); return; }
    const char *err = NULL;
    int64_t got = 0, length = 0;
    // 中途卡死或断开时用 Range 从已下载处续传，而不是结束这首歌（12 dBm 上行偶尔会让连接卡住几十秒）。
    // On a mid-stream stall or drop, resume with Range from the bytes already fetched instead of ending
    // the track (weak 12 dBm uplink occasionally freezes a connection for tens of seconds).
    for (int attempt = 0; attempt <= STREAM_RETRIES && atomic_load(&s_gen) == gen; ++attempt) {
        if (attempt) {
            ESP_LOGW(TAG, "stream stalled at %lld/%lld bytes, resuming (%d/%d)", (long long)got, (long long)length,
                     attempt, STREAM_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
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
                        if (atomic_load(&s_gen) != gen) break;  // 已换歌 / track changed
                        // 下载间隙处理拉列表；换歌请求留给主循环。/ Serve list loads between chunks; leave track changes to the loop.
                        net_req_t req;
                        if (xQueuePeek(s_net_q, &req, 0) == pdTRUE && req.type == NET_LIST) {
                            xQueueReceive(s_net_q, &req, 0);
                            load_list(&req);
                        }
                        sent += xStreamBufferSend(s_ring, chunk + sent, (size_t)n - sent, pdMS_TO_TICKS(100));
                    }
                    if (atomic_load(&s_gen) != gen) break;
                }
            }
        }
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        if (!retry) break;
    }
    if (atomic_load(&s_gen) == gen) dl_finish(gen, err);
}

static void stream_track(const net_req_t *req) {
    if (atomic_load(&s_gen) != req->gen) return;
    // 清掉上一首的残余；解码任务正阻塞读取时复位会失败，稍后重试。
    // Drop the previous track's leftovers; reset fails while the decoder blocks on a read, so retry.
    while (xStreamBufferReset(s_ring) != pdPASS && atomic_load(&s_gen) == req->gen) vTaskDelay(pdMS_TO_TICKS(5));
    atomic_store(&s_content_length, 0);
    atomic_store(&s_ready_gen, req->gen);
    char err[64] = {0}, path[MUSIC_QUERY_MAX + 16];
    if (!media_net_acquire(err, sizeof(err), NULL)) { dl_finish(req->gen, err); return; }
    // I2S 的 DMA 缓冲只能在内部 RAM。WiFi 要约 28 KB，先开 I2S 会让 WiFi 起不来；而 WiFi 刚连上、
    // 还没开始下载时，碎片里仍放得下 I2S 的几小块缓冲。所以连上网就立刻占住 I2S，再开始下载。
    // 万一仍分不到，就断网、先开 I2S、再重连。
    // I2S DMA buffers must be internal RAM. WiFi needs ~28 KB, so opening I2S first starves WiFi; right
    // after WiFi connects, before any download, the small I2S buffers still fit. Claim I2S then, before
    // streaming; failing that, drop WiFi, open I2S first and reconnect.
    if (!metalio_audio_is_open() && metalio_audio_open(AUDIO_RATE, false) != ESP_OK) {
        ESP_LOGW(TAG, "I2S did not fit beside WiFi; reconnecting after claiming it");
        media_net_drop();
        if (metalio_audio_open(AUDIO_RATE, false) != ESP_OK) { dl_finish(req->gen, "音频设备不可用"); return; }
        if (!media_net_acquire(err, sizeof(err), NULL)) { dl_finish(req->gen, err); return; }
    }
    open_uplink();  // 预热过就立即返回 / returns at once after a warm-up
    snprintf(path, sizeof(path), "/url?id=%s", req->text);
    char *body = api_get(path, err, sizeof(err));
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    if (body) heap_caps_free(body);
    const char *url = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "url")) : NULL;
    if (!url) {
        dl_finish(req->gen, err[0] ? err : "无版权或需要会员");
    } else {
        if (cJSON_IsTrue(cJSON_GetObjectItem(root, "trial"))) ESP_LOGW(TAG, "track %s is a trial clip", req->text);
        ESP_LOGI(TAG, "stream %s", req->text);
        stream_url(req->gen, url);
    }
    cJSON_Delete(root);
}

static void net_task(void *arg) {
    (void)arg;
    net_req_t req;
    for (;;) {
        if (xQueueReceive(s_net_q, &req, pdMS_TO_TICKS(NET_POLL_MS)) != pdTRUE) {
            // 空闲：没在播放、也过了保留时间就收回网络并退出，把内部 RAM 还回去。
            // Idle: when not playing and past the hold, release the network and exit to return the internal RAM.
            if (music_active()) continue;
            const int64_t idle_ms = (esp_timer_get_time() - atomic_load(&s_last_req_us)) / 1000;
            if (idle_ms < (atomic_load(&s_page_open) ? NET_HOLD_MS : NET_IDLE_MS)) continue;
            StreamBufferHandle_t ring = NULL;
            taskENTER_CRITICAL(&s_net_mux);
            const bool empty = uxQueueMessagesWaiting(s_net_q) == 0 && !music_active();
            if (empty) { s_net_task = NULL; ring = s_ring; s_ring = NULL; }
            taskEXIT_CRITICAL(&s_net_mux);
            if (!empty) continue;
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
            // 只处理最新的列表请求。/ Only the latest list request matters.
            net_req_t next;
            while (xQueuePeek(s_net_q, &next, 0) == pdTRUE && next.type == NET_LIST) xQueueReceive(s_net_q, &req, 0);
            load_list(&req);
            // 不在这里收网：联网任务在期间算作占用，空闲退出时再收（见 music_net_busy）。
            // No release here: the network counts as in use while this task lives; it is released when the task exits idle.
        } else {
            stream_track(&req);
        }
    }
}

// 投递联网请求，必要时以内部 RAM 栈创建联网任务。/ Post a network request, creating the task with an internal stack if needed.
static void net_post(const net_req_t *req) {
    atomic_store(&s_last_req_us, esp_timer_get_time());
    xQueueSend(s_net_q, req, pdMS_TO_TICKS(200));
    taskENTER_CRITICAL(&s_net_mux);
    const bool need = s_net_task == NULL;
    if (need) s_net_task = (TaskHandle_t)1;  // 占位，防止并发重复创建 / placeholder against double creation
    taskEXIT_CRITICAL(&s_net_mux);
    if (!need) return;
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
        taskENTER_CRITICAL(&s_net_mux);
        s_net_task = NULL;
        taskEXIT_CRITICAL(&s_net_mux);
        return;
    }
    taskENTER_CRITICAL(&s_net_mux);
    if (s_net_task == (TaskHandle_t)1) s_net_task = task;
    taskEXIT_CRITICAL(&s_net_mux);
}

// ---------------------------------------------------------------- decoder

static bool prepare_output(int hz) {
    if (s_out_rate && s_in_rate == hz) return true;
    // BH1098G 时钟固定 16 kHz，所有文件都重采样到它。I2S 通常已由联网任务在开 WiFi 前打开。
    // The BH1098G clock is fixed at 16 kHz and every file is resampled to it. The network task
    // normally opened I2S before starting WiFi.
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
    if (atomic_load(&s_ready_gen) != gen) { vTaskDelay(pdMS_TO_TICKS(20)); return FRAME_WAIT; }
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
    metalio_audio_write(out, (size_t)n, pdMS_TO_TICKS(500));
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

// 播放时才分配环形缓冲，联网任务空闲退出时释放。/ Allocate the ring only for playback; the idle network task frees it.
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
    net_post(&req);
}

static void stop_playback(void) {
    atomic_fetch_add(&s_gen, 1);  // 下载任务放弃当前流 / the downloader abandons its stream
    metalio_audio_close();
    s_out_rate = 0;
    set_state(MUSIC_STOPPED);
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
        const bool running = s_st.state == MUSIC_PLAYING || s_st.state == MUSIC_BUFFERING;
        if (xQueueReceive(s_cmd_q, &cmd, running ? 0 : portMAX_DELAY) == pdTRUE) {
            handle(&cmd);
            continue;
        }
        const frame_result_t r = play_frame();
        if (r == FRAME_FAIL || s_output_failed) { s_output_failed = false; stop_playback(); continue; }
        if (r != FRAME_END) continue;
        // 曲目结束。下载出错（无版权、网络）自动跳过；整轮都失败或没联网就停止。
        // End of track. Skip download errors (unavailable, network); stop after a whole failed round or without WiFi.
        const bool failed = s_samples == 0 && s_dl_error[0];
        if (failed) {
            set_error(s_dl_error);
            ESP_LOGW(TAG, "track %d failed: %s", s_st.queue_index, s_dl_error);
            if (++s_skips >= s_st.queue_count || strstr(s_dl_error, "WiFi") || strstr(s_dl_error, "服务")) {
                stop_playback();
                continue;
            }
        }
        start_track(s_st.mode == MUSIC_REPEAT_ONE && !failed ? s_st.queue_index : pick_next(s_st.queue_index, 1));
    }
}

// ---------------------------------------------------------------- public API

static void post(cmd_type_t type, int arg, bool wait) {
    if (!s_cmd_q) return;
    cmd_t cmd = {.type = type, .arg = arg, .done = wait ? xSemaphoreCreateBinary() : NULL};
    if (xQueueSend(s_cmd_q, &cmd, pdMS_TO_TICKS(200)) == pdTRUE && cmd.done)
        xSemaphoreTake(cmd.done, pdMS_TO_TICKS(3000));
    if (cmd.done) vSemaphoreDelete(cmd.done);
}

esp_err_t music_init(void) {
    if (s_cmd_q) return ESP_OK;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(8, sizeof(cmd_t));
    s_net_q = xQueueCreate(6, sizeof(net_req_t));
    s_list = heap_caps_calloc(MUSIC_PAGE_SIZE, sizeof(music_track_t), caps);
    s_queue = heap_caps_calloc(MUSIC_PAGE_SIZE, sizeof(music_track_t), caps);
    s_dec = heap_caps_malloc(sizeof(mp3dec_t), caps);
    s_in = heap_caps_malloc(IN_BUF_BYTES, caps);
    s_pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), caps);
    s_out = heap_caps_malloc(OUT_MAX * sizeof(int16_t), caps);
    if (!s_lock || !s_cmd_q || !s_net_q || !s_list || !s_queue ||
        !s_dec || !s_in || !s_pcm || !s_out)
        return ESP_ERR_NO_MEM;
    s_st.state = MUSIC_STOPPED;
    s_st.queue_index = -1;
    s_st.list_total = -1;
    s_st.volume = metalio_audio_volume();
    // minimp3 栈上约 16 KB；解码任务不碰 flash，栈放 PSRAM。联网任务按需创建。
    // minimp3 needs ~16 KB of stack; the decoder never touches flash, so its stack lives in PSRAM.
    // The network task is created on demand.
    if (xTaskCreatePinnedToCoreWithCaps(decode_task, "music_dec", 24 * 1024, NULL, 6, NULL, 1, caps) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void music_load(music_source_t source, const char *query, int offset) {
    if (!s_net_q || source >= MUSIC_SRC_COUNT) return;
    net_req_t req = {.type = NET_LIST, .source = source, .offset = offset < 0 ? 0 : offset};
    snprintf(req.text, sizeof(req.text), "%s", query ? query : "");
    lock();
    s_st.list_source = source;
    s_st.list_offset = req.offset;
    s_st.list_loading = true;
    s_st.list_error[0] = 0;
    snprintf(s_st.query, sizeof(s_st.query), "%s", req.text);
    unlock();
    net_post(&req);
}

void music_page_open(bool open) {
    atomic_store(&s_page_open, open);
    if (!open || !s_net_q) return;
    if (!music_active()) {
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
bool music_net_busy(void) {
    // 联网任务还在（含音乐页开着时的保留期）也算占用，免得 AI 结束时把预热好的 WiFi 收掉。
    // The network task being alive (including the hold while the music page is open) counts as use, so an
    // AI chat ending does not drop the warmed-up WiFi.
    return s_st.state != MUSIC_STOPPED || s_st.list_loading || s_net_task != NULL;
}
