/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：网易云在线播放。歌单与播放地址来自 esp/tools/music-server（Bearer 令牌），MP3 流经
 * PSRAM 环形缓冲由 minimp3 解码。列表、下载、解码各一个任务，页面只读状态、发命令；
 * 离开音乐页仍可后台播放。
 *
 * English: NetEase online playback. Lists and play URLs come from esp/tools/music-server (Bearer
 * token); the MP3 stream goes through a PSRAM ring buffer into minimp3. Listing, downloading and
 * decoding each run in their own task; pages only read status and post commands, and playback
 * continues in the background after leaving the music page.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_PAGE_SIZE 30
#define MUSIC_NAME_MAX 96
#define MUSIC_ARTIST_MAX 64
#define MUSIC_QUERY_MAX 64

typedef enum { MUSIC_SRC_TOP, MUSIC_SRC_LIKES, MUSIC_SRC_DAILY, MUSIC_SRC_SEARCH, MUSIC_SRC_COUNT } music_source_t;
typedef enum { MUSIC_STOPPED, MUSIC_BUFFERING, MUSIC_PLAYING, MUSIC_PAUSED } music_state_t;
typedef enum { MUSIC_REPEAT_ALL, MUSIC_REPEAT_ONE, MUSIC_SHUFFLE } music_mode_t;

typedef struct {
    char id[24];
    char name[MUSIC_NAME_MAX];
    char artist[MUSIC_ARTIST_MAX];
} music_track_t;

typedef struct {
    // 播放 / playback
    music_state_t state;
    music_mode_t mode;
    int queue_index;          // 播放队列中的位置，-1 表示无 / position in the play queue, -1 for none
    int queue_count;
    music_source_t queue_source;
    int queue_offset;         // 队列对应的列表偏移 / list offset the queue was taken from
    int position_s;
    int duration_s;           // 由码率与长度估算，0 表示未知 / estimated from bitrate and length, 0 if unknown
    int buffered_pct;         // 环形缓冲占用 / ring buffer fill, 0–100
    int volume;               // 0–100
    char title[MUSIC_NAME_MAX];
    char artist[MUSIC_ARTIST_MAX];
    char error[64];           // 播放错误 / playback error
    // 列表 / list
    music_source_t list_source;
    int list_offset;
    int list_count;
    int list_total;           // -1 表示未知 / -1 when unknown
    bool list_loading;
    char list_error[64];
    char query[MUSIC_QUERY_MAX];
    unsigned revision;        // 任何变化都递增 / bumps on any change
} music_status_t;

esp_err_t music_init(void);
/// 异步加载列表；source 为 SEARCH 时用 query。/ Load a list asynchronously; query is used for SEARCH.
void music_load(music_source_t source, const char *query, int offset);
/// 音乐页打开/离开。打开时在后台连上 WiFi 并打通上行；页面开着时联网保留到最后一次操作后 90 秒。
/// The music page opened or left. On open, join WiFi and open the uplink in the background; while the
/// page is open the network is kept until 90 s after the last request.
void music_page_open(bool open);
/// 当前列表中的一项。/ One entry of the current list.
bool music_list_track(int index, music_track_t *out);
/// 用当前列表作为播放队列，从 index 开始播放。/ Use the current list as the queue and play from index.
void music_play(int index);
void music_toggle(void);
void music_next(void);
void music_prev(void);
void music_stop(void);
void music_set_volume(int volume);
void music_cycle_mode(void);
void music_get_status(music_status_t *out);
/// 播放、缓冲或暂停中（占用音频与网络）。/ Playing, buffering or paused (holding audio and network).
bool music_active(void);
/// 播放中或正在拉列表（需要网络）。/ Playing or loading a list (needs the network).
bool music_net_busy(void);

#ifdef __cplusplus
}
#endif
