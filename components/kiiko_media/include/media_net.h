/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：音乐与 AI 语音共用的联网管理。只连接已保存的 WiFi；本模块启动的连接由本模块在两者都不用时收回。
 * 会启动/停止 WiFi（读写 NVS、关缓存），只能从栈在内部 RAM 的任务调用，不能用 PSRAM 栈。
 *
 * English: Network ownership shared by music and AI voice. Joins the saved WiFi only; a connection this
 * module started is released once neither needs it. Starting/stopping WiFi touches NVS with the cache
 * off, so call only from tasks whose stack is internal RAM, never a PSRAM stack.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 使用网络的一方。每方各占一位：acquire 时置上，release 时清掉，都清掉了才收回本模块启动的 WiFi。
/// A user of the network. Each holds one bit: set on acquire, cleared on release; WiFi this module started
/// is released only once no bit is left.
typedef enum {
    MEDIA_NET_MUSIC = 1 << 0,
    MEDIA_NET_AI = 1 << 1,
} media_net_client_t;

/// client 占用网络，需要时连上已保存的 WiFi（最多约 30 秒）；失败写 err。cancel 非空且变为真时立即放弃。只认 STA，
/// 热点传书不抢。不论成败都算占着，用完必须 media_net_release。
/// client takes the network, joining the saved WiFi if needed (up to ~30 s); writes err on failure. Gives up
/// at once when a non-NULL cancel turns true. Only an STA counts; a transfer hotspot is never taken over.
/// Counts as held whether or not it succeeds: always pair it with media_net_release.
bool media_net_acquire(media_net_client_t client, char *err, size_t err_size, const volatile bool *cancel);
/// 为先占 I2S 而在空闲时断开 STA，调用方随后重新 acquire。先等其他方放手（等通知，最多 wait_ms），等不到也照断；
/// wait_ms 为 0 时立即断。断的若是别人开的 WiFi，重连后收回时保持开着。
/// Drop the STA when idle, to claim I2S first; the caller acquires again afterwards. First waits (woken by
/// their release, at most wait_ms) for the other clients to let go, and drops anyway after that; 0 drops at
/// once. If it was someone else's WiFi, the rejoined connection stays up at release.
void media_net_drop(media_net_client_t client, int wait_ms);
/// client 不再需要网络。没有任何一方占着时，断开本模块启动、且仍是当前会话的 WiFi。会停 WiFi，限制同上。
/// client no longer needs the network. Once no client holds it, drop WiFi this module started if it is
/// still the current session. May stop WiFi, with the restriction above.
void media_net_release(media_net_client_t client);
/// client 放手但不停 WiFi（它还可能压着驱动缓冲时用）：WiFi 留到下一位使用方 release 时再收。
/// client lets go without stopping WiFi (when it may still hold driver buffers): WiFi stays until the next
/// client's release.
void media_net_forget(media_net_client_t client);
/// 没有任何一方占着网络（最后一次 release 的收回已做完）。/ No client holds the network (the last release has finished).
bool media_net_idle(void);
/// 等到没有任何一方占着网络（等放手通知），最多 wait_ms；返回是否已空闲。
/// Wait up to wait_ms (woken by the releases) until no client holds the network; returns whether it is idle.
bool media_net_wait_idle(int wait_ms);

/// 向网关的丢弃端口发 30 个 1 字节 UDP 包，推通刚连上 WiFi 时卡住的上行（见 media_net.c）。sent/failed 可为 NULL。
/// Send 30 one-byte UDP packets to the gateway's discard port to open the uplink that stalls right
/// after joining WiFi (see media_net.c). sent/failed may be NULL.
void media_net_uplink_nudge(int *sent, int *failed);

/// 这次连上 WiFi 后还没打通过上行时：推上行并向 host:port 建一次 TCP 连接，没连上就每秒再推，连通即关闭。
/// 最多 8 秒；cancel 变真或 keep 返回 false 时立即停。已打通过就直接返回。返回打通用的毫秒数，
/// 已打通返回 0，没打通返回 -1。会阻塞，只在联网任务里调用。
/// Once per WiFi connection: push the uplink and open one TCP connection to host:port, pushing again
/// each second until it connects, then close it. At most 8 s; stops at once when cancel turns true or
/// keep returns false. Returns at once if already open. Returns the milliseconds it took, 0 if already
/// open, -1 if not opened. Blocks; call from a network task only.
int media_net_ensure_uplink(const char *host, int port, const volatile bool *cancel, bool (*keep)(void));

#ifdef __cplusplus
}
#endif
