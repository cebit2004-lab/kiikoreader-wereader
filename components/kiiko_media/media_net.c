/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：见 media_net.h。/ English: see media_net.h.
 */
#include "media_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "ai_voice.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "music_player.h"
#include "read_pico_transfer.h"

static const char *TAG = "media_net";
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static bool s_owns;

static void take(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

bool media_net_acquire(char *err, size_t err_size, const volatile bool *cancel) {
    take();
    read_pico_transfer_status_t st;
    read_pico_transfer_get_status(&st);
    bool ok = st.network_ready;
    if (!ok) {
        char ssid[33];
        bool configured = false;
        read_pico_transfer_get_saved_wifi(ssid, &configured);
        if (!configured) {
            snprintf(err, err_size, "请先在设置里连接 WiFi");
        } else {
            // 自己发起的连接还在进行（如 AI 页预热被打断后）：接着等，不要拆了重来。
            // A connection we started is still in progress (say, an interrupted AI warm-up): keep
            // waiting instead of tearing it down and starting over.
            bool waiting = s_owns && st.state == READ_PICO_TRANSFER_STARTING;
            if (!waiting) {
                if (st.state != READ_PICO_TRANSFER_STOPPED) read_pico_transfer_stop();
                const read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
                waiting = read_pico_transfer_start(&cfg) == ESP_OK;
                if (waiting) s_owns = true;
            }
            if (waiting) {
                for (int i = 0; i < 300 && !ok && !(cancel && *cancel); ++i) {
                    read_pico_transfer_service_poll();
                    read_pico_transfer_get_status(&st);
                    ok = st.network_ready;
                    if (!ok) vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            if (!ok) snprintf(err, err_size, cancel && *cancel ? "已取消" : "WiFi 连接失败");
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/*
 * 旧 AI 连接的任务还在收尾时先等它退出再停 WiFi：它的套接字可能还压着驱动的接收缓冲，驱动卸载后
 * 再归还就是访问已释放的内存。它卡在 TCP 连接阶段时套接字关不掉，最长约等于连接超时（20 秒）。
 * Before stopping WiFi, wait for an old AI connection's task to exit: its socket may still hold driver
 * receive buffers, and returning them after the driver is gone touches freed memory. A task stuck in
 * the TCP connect cannot have its socket shut, so this can take up to the connect timeout (20 s).
 */
static void wait_ai_net_idle(void) {
    if (!ai_net_busy()) return;
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 250 && ai_net_busy(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (ai_net_busy()) ESP_LOGW(TAG, "old AI connection still open after %d ms; stopping WiFi anyway", ms);
    else ESP_LOGI(TAG, "waited %d ms for the old AI connection to close", ms);
}

void media_net_drop(void) {
    wait_ai_net_idle();
    take();
    read_pico_transfer_stop();
    s_owns = false;
    xSemaphoreGive(s_lock);
}

void media_net_release_if_unused(void) {
    take();
    // 旧 AI 连接还在收尾时不收：AI 的控制任务等它退出后会再调一次。
    // Not while an old AI connection is shutting down: the AI control task calls again once it exits.
    if (s_owns && !music_net_busy() && !ai_active() && !ai_net_busy()) {
        read_pico_transfer_stop();
        s_owns = false;
        ESP_LOGI(TAG, "network released");
    }
    xSemaphoreGive(s_lock);
}

/*
 * 刚连上 WiFi 时的上行。2026-11-24/26 实测：拿到地址后，本机发出的包（包括发往网关的）会全部丢失，收包
 * 一直正常；只等不发，8 秒内连接请求一个回应都没有；集中发出一批包后，重发的连接请求 80 ms 就有回应。
 * 推断是加密包序号在连上后被重置，对端把序号偏小的包当重放丢弃，直到本机发出的包够多。所以每次连上
 * WiFi 后先推一批包，再用一次 TCP 连接确认打通。AI 和音乐共用。
 * The uplink right after joining WiFi. Measured 2026-11-24/26: after getting an address, every packet
 * this device sends (to the gateway too) is lost while receiving works; waiting without sending, not
 * one connection request was answered in 8 s; after a batch of packets went out, the retransmitted
 * request was answered 80 ms later. Likely the encrypted packet number is reset on joining and the
 * peer drops lower-numbered packets as replays until enough have been sent. So after each join, push a
 * batch and confirm with one TCP connection. Shared by AI and music.
 */
#define UPLINK_MAX_MS 8000
static volatile bool s_uplink_open;
static bool s_ip_handler;

// 每次拿到新地址（新连接或重连）都要重新打通。/ Every new address (join or rejoin) needs the uplink opened again.
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    s_uplink_open = false;
}

void media_net_uplink_nudge(int *sent_out, int *failed_out) {
    int sent = 0, failed = 0;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    const int fd = sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.gw.addr ? socket(AF_INET, SOCK_DGRAM, 0) : -1;
    if (fd >= 0) {
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(9)};
        addr.sin_addr.s_addr = info.gw.addr;
        const uint8_t byte = 0;
        for (int i = 0; i < 30; ++i) {
            if (sendto(fd, &byte, 1, 0, (struct sockaddr *)&addr, sizeof(addr)) == 1) ++sent;
            else ++failed;
            vTaskDelay(pdMS_TO_TICKS(5));  // 留时间给驱动发出去，免得发送缓冲一下占满 / let the driver drain its few TX buffers
        }
        close(fd);
    }
    if (sent_out) *sent_out = sent;
    if (failed_out) *failed_out = failed;
}

int media_net_ensure_uplink(const char *host, int port, const volatile bool *cancel, bool (*keep)(void)) {
    if (!s_ip_handler && esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL) == ESP_OK)
        s_ip_handler = true;
    if (s_uplink_open) return 0;
    const int64_t t0 = esp_timer_get_time();
#define STOPPED() ((cancel && *cancel) || (keep && !keep()))
#define ELAPSED_MS() ((int)((esp_timer_get_time() - t0) / 1000))
    int sent = 0, failed = 0;
    media_net_uplink_nudge(&sent, &failed);
    int64_t nudged = esp_timer_get_time();
    int nudges = 1;
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char service[8];
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "uplink: dns %s failed", host);
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    // 连通即复位关闭，不占服务器名额。/ Reset on close so it holds no server slot.
    const struct linger lg = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    const int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    bool ok = r == 0;
    const char *how = ok ? "connected" : "failed";
    if (!ok && errno == EINPROGRESS) {
        how = "timed out";
        // 分段等，好及时响应取消；没连上就每秒再推一次。/ Wait in slices to honour cancel; push again each second until connected.
        while (!STOPPED() && ELAPSED_MS() < UPLINK_MAX_MS) {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(fd, &wr);
            struct timeval wait = {.tv_sec = 0, .tv_usec = 200 * 1000};
            if (select(fd + 1, NULL, &wr, NULL, &wait) == 1) {
                int soerr = -1;
                socklen_t len = sizeof(soerr);
                ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0;
                how = ok ? "connected" : "failed";
                break;
            }
            if (esp_timer_get_time() - nudged > 1000000) {
                media_net_uplink_nudge(NULL, NULL);
                nudged = esp_timer_get_time();
                ++nudges;
            }
        }
        if (!ok && STOPPED()) how = "stopped";
    }
    close(fd);
    if (ok) s_uplink_open = true;
    ESP_LOGI(TAG, "uplink to %s %s after %d ms (%d pushes, first %d sent %d refused)", host, how, ELAPSED_MS(),
             nudges, sent, failed);
    return ok ? (ELAPSED_MS() > 0 ? ELAPSED_MS() : 1) : -1;
#undef STOPPED
#undef ELAPSED_MS
}
