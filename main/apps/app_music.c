/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：网易云音乐页。上方四个入口（热歌榜 / 我喜欢 / 每日推荐 / 搜索），中间歌曲列表，底部正在播放与控制。
 * 播放在 music_player 后台任务里，离开本页继续。进度每 5 秒局部刷新；搜索用系统拼音键盘，只局部刷键盘区。
 * 音量键调音量，中键返回，中键长按全刷。
 *
 * English: NetEase music page. Four sources on top (hot chart / liked / daily / search), the song list
 * in the middle, now-playing and controls at the bottom. Playback lives in the music_player tasks and
 * continues after leaving. Progress refreshes locally every 5 s; search uses the system Pinyin
 * keyboard with local keyboard repaints. Volume keys set the volume, the middle key returns, and a
 * middle hold forces a full refresh.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_registry.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "music_player.h"
#include "ui_gesture.h"
#include "ui_keyboard.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_text_edit.h"
#include "ui_text_input.h"

#define ROWS 8
#define ROW_H 72
#define TAB_TOP 168
#define TAB_H 72
#define LIST_TOP 262
#define PAGER_TOP 846
#define PLAYER_TOP 930
#define FIELD ((EpdRect){36, 242, 612, 82})

typedef enum { VIEW_LIST, VIEW_SEARCH } view_t;


// 返回：从「应用」页进来的回「应用」，否则回首页（不进总菜单）。
// Back: to the Apps page when opened from it, otherwise home (never the root menu).
extern const app_desc_t *g_apps_origin;
static void go_back(app_ctx_t *ctx) {
    ctx->request_app = g_apps_origin ? g_apps_origin : app_home_page();
    g_apps_origin = NULL;
}

static view_t s_view;
static int s_page;
// 从「应用」页卡片上的入口进来时要打开的来源，-1 为没有。/ Source picked on the Apps card, -1 for none.
static int s_entry_source = -1;
void app_music_open_source(int source) { s_entry_source = source; }
static unsigned s_drawn_sig;
static int s_drawn_pos;
static char s_query_draft[MUSIC_QUERY_MAX];
static ui_text_edit_t s_edit;
static EpdRect s_input_area;
static bool s_input_layout, s_input_settle;

static const char *const k_tabs[MUSIC_SRC_COUNT] = {"热歌榜", "我喜欢", "每日推荐", "搜索"};

static EpdRect tab_rect(int i) { return (EpdRect){36 + i * 153, TAB_TOP, 153, TAB_H}; }
static EpdRect ctrl_rect(int i) { return (EpdRect){36 + i * 156, 1100, 144, 88}; }
static EpdRect progress_rect(void) { return (EpdRect){36, 1032, 612, 52}; }

static void fmt_time(char *out, size_t size, int s) {
    if (s < 0) s = 0;
    snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

// 除播放进度外的状态签名；变化即整页重绘。/ Status signature excluding position; a change redraws the page.
static unsigned status_sig(const music_status_t *st) {
    unsigned h = (unsigned)st->state * 131u + (unsigned)st->mode * 7u + (unsigned)(st->queue_index + 1) * 1009u +
                 (unsigned)st->volume * 31u + (unsigned)st->list_count * 65537u + (unsigned)st->list_source * 977u +
                 (unsigned)st->list_offset * 13u + (st->list_loading ? 0x80000000u : 0) + (unsigned)st->duration_s * 3u;
    for (const char *p = st->error; *p; ++p) h = h * 33u + (unsigned char)*p;
    for (const char *p = st->list_error; *p; ++p) h = h * 33u + (unsigned char)*p;
    return h;
}

static int local_pages(const music_status_t *st) { return st->list_count > 0 ? (st->list_count + ROWS - 1) / ROWS : 1; }
static bool has_next_batch(const music_status_t *st) {
    if (st->list_source == MUSIC_SRC_SEARCH || st->list_source == MUSIC_SRC_DAILY) return false;
    if (st->list_total >= 0) return st->list_offset + st->list_count < st->list_total;
    return st->list_count >= MUSIC_PAGE_SIZE;
}

static void draw_progress(uint8_t *fb, const music_status_t *st) {
    EpdRect r = progress_rect();
    epd_fill_rect(r, UI_GRAY_WHITE, fb);
    EpdRect bar = {r.x + 90, r.y + 20, r.width - 180, 10};
    ui_fill_round_rect(fb, bar, 5, 0xd0);
    if (st->duration_s > 0) {
        int w = (int)((int64_t)bar.width * st->position_s / st->duration_s);
        if (w > bar.width) w = bar.width;
        if (w > 10) ui_fill_round_rect(fb, (EpdRect){bar.x, bar.y, w, bar.height}, 5, 0x30);
    }
    char pos[16], dur[16];
    fmt_time(pos, sizeof(pos), st->position_s);
    if (st->duration_s > 0) fmt_time(dur, sizeof(dur), st->duration_s); else snprintf(dur, sizeof(dur), "--:--");
    ui_text_box(fb, (EpdRect){r.x, r.y + 6, 84, 40}, 22, pos, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){r.x + r.width - 84, r.y + 6, 84, 40}, 22, dur, EPD_DRAW_ALIGN_RIGHT, false);
}

static void render_search(uint8_t *fb) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_box(fb, (EpdRect){120, 79, 380, 66}, 34, "搜索歌曲", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_box(fb, (EpdRect){530, 79, 118, 66}, 26, "搜索", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_input_draw(fb, &s_edit, FIELD, 29, false, NULL);
    ui_text_box(fb, (EpdRect){36, 352, 612, 60}, 22, "输入歌名或歌手，可用拼音", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, (EpdRect){36, 440, 180, 66}, "清空", false);
    ui_keyboard_draw(fb, UI_SYSTEM_KEYBOARD_TOP);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_view == VIEW_SEARCH) { render_search(fb); return; }
    music_status_t st;
    music_get_status(&st);
    s_drawn_sig = status_sig(&st);
    s_drawn_pos = st.position_s;

    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "网易云音乐", EPD_DRAW_ALIGN_CENTER, false);

    // 来源标签。/ Source tabs.
    for (int i = 0; i < MUSIC_SRC_COUNT; ++i) {
        EpdRect r = ui_inset_rect(tab_rect(i), 4);
        const bool on = st.list_source == (music_source_t)i;
        if (on) ui_fill_round_rect(fb, r, 14, 0x30);
        else ui_draw_round_rect(fb, r, 14, 0x90);
        ui_text_box(fb, ui_inset_rect(r, 8), 28, k_tabs[i], EPD_DRAW_ALIGN_CENTER, on);
    }

    // 列表。/ Song list.
    const int pages = local_pages(&st);
    if (s_page >= pages) s_page = pages - 1;
    if (s_page < 0) s_page = 0;
    const char *notice = NULL;
    char search_line[MUSIC_QUERY_MAX + 32];
    if (st.list_source == MUSIC_SRC_SEARCH) {
        snprintf(search_line, sizeof(search_line), st.query[0] ? "搜索：%s（点此修改）" : "点此输入要搜的歌", st.query);
        ui_text_box(fb, (EpdRect){36, LIST_TOP - 10, 612, 50}, 24, search_line, EPD_DRAW_ALIGN_LEFT, false);
    }
    const int list_top = st.list_source == MUSIC_SRC_SEARCH ? LIST_TOP + 48 : LIST_TOP;
    const int rows = st.list_source == MUSIC_SRC_SEARCH ? ROWS - 1 : ROWS;
    if (st.list_loading) notice = "正在加载…";
    else if (st.list_error[0]) notice = st.list_error;
    else if (st.list_source == MUSIC_SRC_SEARCH && !st.query[0]) notice = "";
    if (notice) {
        ui_text_box(fb, (EpdRect){52, list_top + 160, 580, 60}, 28, notice, EPD_DRAW_ALIGN_CENTER, false);
    } else {
        for (int row = 0; row < rows; ++row) {
            const int index = s_page * ROWS + row;
            music_track_t t;
            if (!music_list_track(index, &t)) break;
            EpdRect r = {36, list_top + row * ROW_H, 612, ROW_H};
            const bool playing = st.queue_index == index && st.queue_source == st.list_source &&
                                 st.queue_offset == st.list_offset && st.state != MUSIC_STOPPED;
            if (playing) ui_fill_round_rect(fb, (EpdRect){r.x, r.y + 3, r.width, r.height - 6}, 10, 0xe0);
            char line[MUSIC_NAME_MAX + MUSIC_ARTIST_MAX + 24];
            snprintf(line, sizeof(line), "%d. %s", st.list_offset + index + 1, t.name);
            ui_text_box(fb, (EpdRect){r.x + 12, r.y + 6, 600 - 12, 38}, 27, line, EPD_DRAW_ALIGN_LEFT, false);
            ui_text_box(fb, (EpdRect){r.x + 12, r.y + 44, 600 - 12, 24}, 19, t.artist, EPD_DRAW_ALIGN_LEFT, false);
            ui_hairline(fb, r.y + r.height - 1, r.x, r.width, 0xdc);
        }
    }
    char label[48];
    const int batch = st.list_offset / MUSIC_PAGE_SIZE + 1;
    if (st.list_offset > 0 || has_next_batch(&st)) snprintf(label, sizeof(label), "第%d批 %d/%d", batch, s_page + 1, pages);
    else snprintf(label, sizeof(label), "%d/%d", s_page + 1, pages);
    // 前后还有批次时让分页箭头保持可点。/ Keep the pager arrows live when earlier or later batches exist.
    const int before = st.list_offset > 0 ? 1 : 0, after = has_next_batch(&st) ? 1 : 0;
    if (!notice) ui_pager_draw_at(fb, label, s_page + before, pages + before + after, false, -1, PAGER_TOP);

    // 正在播放。/ Now playing.
    ui_hairline(fb, PLAYER_TOP - 14, 0, 684, 0x90);
    const char *state = st.state == MUSIC_PLAYING ? "正在播放" : st.state == MUSIC_BUFFERING ? "缓冲中"
                      : st.state == MUSIC_PAUSED ? "已暂停" : "未播放";
    char head[MUSIC_NAME_MAX + 32];
    if (st.queue_index >= 0) snprintf(head, sizeof(head), "%s", st.title);
    else snprintf(head, sizeof(head), "点上方歌曲开始播放");
    ui_text_box(fb, (EpdRect){36, PLAYER_TOP, 612, 46}, 32, head, EPD_DRAW_ALIGN_LEFT, false);
    char sub[160];
    if (st.error[0]) snprintf(sub, sizeof(sub), "%s · %s", state, st.error);
    else if (st.queue_index >= 0) snprintf(sub, sizeof(sub), "%s · %s · 音量 %d", state, st.artist, st.volume);
    else snprintf(sub, sizeof(sub), "%s · 音量 %d", state, st.volume);
    ui_text_box(fb, (EpdRect){36, PLAYER_TOP + 50, 612, 36}, 22, sub, EPD_DRAW_ALIGN_LEFT, false);
    draw_progress(fb, &st);
    static const char *const modes[3] = {"列表循环", "单曲循环", "随机播放"};
    const char *labels[4] = {"上一首", st.state == MUSIC_PLAYING || st.state == MUSIC_BUFFERING ? "暂停" : "播放",
                             "下一首", modes[st.mode]};
    for (int i = 0; i < 4; ++i) ui_draw_button(fb, ctrl_rect(i), labels[i], i == 1);
}

// ---------------------------------------------------------------- search view

static app_redraw_t search_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, UI_SYSTEM_KEYBOARD_TOP, FIELD, 29, false, NULL, field);
    s_input_area = update.area;
    s_input_layout = update.layout;
    s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}

static void open_search(void) {
    music_status_t st;
    music_get_status(&st);
    snprintf(s_query_draft, sizeof(s_query_draft), "%s", st.query);
    ui_text_edit_init(&s_edit, s_query_draft, sizeof(s_query_draft));
    ui_keyboard_begin(&s_edit, false);
    s_view = VIEW_SEARCH;
}

static app_redraw_t apply_search(void) {
    if (ui_keyboard_pending()) return APP_REDRAW_NONE;  // 拼音未确认 / Pinyin not committed yet
    ui_keyboard_end();
    s_view = VIEW_LIST;
    s_page = 0;
    if (s_query_draft[0]) music_load(MUSIC_SRC_SEARCH, s_query_draft, 0);
    return APP_REDRAW_FULL;
}

static app_redraw_t search_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms) ? search_paint(ctx, false) : APP_REDRAW_NONE;
    const bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        const bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? search_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? search_paint(ctx, false) : APP_REDRAW_NONE;
    const int x = ev->x0, y = ev->y0;
    if (y < 160) {
        if (x > 510) return apply_search();
        if (x < 160) { ui_keyboard_end(); s_view = VIEW_LIST; return APP_REDRAW_FULL; }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit((EpdRect){36, 440, 180, 66}, x, y)) {
        s_query_draft[0] = 0;
        ui_text_edit_init(&s_edit, s_query_draft, sizeof(s_query_draft));
        return search_paint(ctx, true);
    }
    if (y < UI_SYSTEM_KEYBOARD_TOP)
        return ui_text_input_tap(&s_edit, FIELD, 29, false, NULL, x, y) ? search_paint(ctx, true) : APP_REDRAW_NONE;
    const ui_keyboard_result_t result = ui_keyboard_tap(x, y, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms);
    if (result == UI_KEYBOARD_DONE) return apply_search();
    return result == UI_KEYBOARD_CHANGED || feedback ? search_paint(ctx, false) : APP_REDRAW_NONE;
}

static bool music_present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (redraw != APP_REDRAW_AREA || s_view != VIEW_SEARCH) return false;
    if (s_input_settle) {
        guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_input_area));
        s_input_settle = false;
        return true;
    }
    guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, s_input_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
                                                             s_input_layout ? MODE_GL16 : MODE_DU, s_input_area));
    return true;
}

// ---------------------------------------------------------------- list view

static void select_source(music_source_t source) {
    s_page = 0;
    if (source == MUSIC_SRC_SEARCH) {
        // 切到搜索只换来源、不发请求（空搜索词不请求服务器）；已在搜索就直接改搜索词。两种情况都打开键盘。
        // Switching to search only changes the source with no request (an empty query never hits the server);
        // already on search, just edit the query. Either way the keyboard opens.
        music_status_t st;
        music_get_status(&st);
        if (st.list_source != MUSIC_SRC_SEARCH) music_load(MUSIC_SRC_SEARCH, "", 0);
        open_search();
        return;
    }
    music_load(source, NULL, 0);
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_view == VIEW_SEARCH) return search_gesture(ctx, ev);
    music_status_t st;
    music_get_status(&st);
    const int pages = local_pages(&st);
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        if (ev->y0 < LIST_TOP || ev->y0 > PAGER_TOP + 60) return APP_REDRAW_NONE;
        if (ev->type == UI_GESTURE_SWIPE_L) {
            if (s_page + 1 < pages) { ++s_page; return APP_REDRAW_PAGE; }
            if (has_next_batch(&st)) { s_page = 0; music_load(st.list_source, st.query, st.list_offset + MUSIC_PAGE_SIZE); return APP_REDRAW_PAGE; }
        } else {
            if (s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
            if (st.list_offset > 0) { s_page = 0; music_load(st.list_source, st.query, st.list_offset - MUSIC_PAGE_SIZE); return APP_REDRAW_PAGE; }
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const uint16_t x = ev->x0, y = ev->y0;
    if (y < 168 && x < 160) { go_back(ctx); return APP_REDRAW_NONE; }  // 稍大的返回区 / a larger back area
    for (int i = 0; i < MUSIC_SRC_COUNT; ++i)
        if (ui_rect_hit(tab_rect(i), x, y)) {
            if ((music_source_t)i == st.list_source && i != MUSIC_SRC_SEARCH && !st.list_error[0]) return APP_REDRAW_NONE;
            select_source((music_source_t)i);
            return s_view == VIEW_SEARCH ? APP_REDRAW_FULL : APP_REDRAW_PAGE;
        }
    if (st.list_source == MUSIC_SRC_SEARCH && y >= LIST_TOP - 10 && y < LIST_TOP + 40) {
        open_search();
        return APP_REDRAW_FULL;
    }
    for (int i = 0; i < 4; ++i)
        if (ui_rect_hit(ctrl_rect(i), x, y)) {
            if (i == 0) music_prev();
            else if (i == 1) music_toggle();
            else if (i == 2) music_next();
            else music_cycle_mode();
            return APP_REDRAW_NONE;
        }
    // 分页条：前后批也走这里。/ Pager, including previous/next batches.
    const int pager = ui_pager_hit_at(x, y, PAGER_TOP);
    if (pager != UI_PAGER_NONE && !st.list_loading) {
        const bool forward = pager == UI_PAGER_NEXT || pager == UI_PAGER_LAST;
        if (forward && s_page + 1 >= pages && has_next_batch(&st)) {
            s_page = 0; music_load(st.list_source, st.query, st.list_offset + MUSIC_PAGE_SIZE); return APP_REDRAW_PAGE;
        }
        if (!forward && s_page == 0 && st.list_offset > 0) {
            s_page = 0; music_load(st.list_source, st.query, st.list_offset - MUSIC_PAGE_SIZE); return APP_REDRAW_PAGE;
        }
        const int target = ui_pager_target(pager, s_page, pages);
        if (target != s_page) { s_page = target; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    const int list_top = st.list_source == MUSIC_SRC_SEARCH ? LIST_TOP + 48 : LIST_TOP;
    const int rows = st.list_source == MUSIC_SRC_SEARCH ? ROWS - 1 : ROWS;
    if (!st.list_loading && y >= list_top && y < list_top + rows * ROW_H) {
        const int index = s_page * ROWS + (y - list_top) / ROW_H;
        if (index < st.list_count) music_play(index);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_view == VIEW_SEARCH) {
        if (key == UI_KEY_1 || key == UI_KEY_3) return APP_REDRAW_NONE;
        ui_keyboard_end();
        s_view = VIEW_LIST;
        return APP_REDRAW_FULL;
    }
    music_status_t st;
    music_get_status(&st);
    // 实机上 UI_KEY_1 对应音量+、UI_KEY_3 对应音量−。/ On the device UI_KEY_1 is volume up and UI_KEY_3 volume down.
    if (key == UI_KEY_1) { music_set_volume(st.volume + 10); return APP_REDRAW_NONE; }
    if (key == UI_KEY_3) { music_set_volume(st.volume - 10); return APP_REDRAW_NONE; }
    go_back(ctx);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    (void)ctx;
    return key == UI_KEY_2 ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_view == VIEW_SEARCH) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        const bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0, ctx->touch ? ctx->touch->y : 0, ctx->now_ms) ==
            UI_KEYBOARD_CHANGED)
            return search_paint(ctx, false);
        return ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED
                   ? search_paint(ctx, false) : APP_REDRAW_NONE;
    }
    music_status_t st;
    music_get_status(&st);
    if (status_sig(&st) != s_drawn_sig) return APP_REDRAW_PAGE;
    if (st.state == MUSIC_PLAYING && (st.position_s - s_drawn_pos >= 5 || st.position_s < s_drawn_pos)) {
        s_drawn_pos = st.position_s;
        draw_progress(ctx->fb, &st);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t *ctx) { (void)ctx; return progress_rect(); }

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_view = VIEW_LIST;
    music_page_open(true);
    const int entry = s_entry_source;
    s_entry_source = -1;
    if (entry == MUSIC_SRC_SEARCH) { open_search(); return; }
    music_status_t st;
    music_get_status(&st);
    if (entry >= 0 && entry < MUSIC_SRC_COUNT && (st.list_source != (music_source_t)entry || st.list_count == 0)) {
        music_load((music_source_t)entry, NULL, 0);
        s_page = 0;
        return;
    }
    // 首次进入或上次失败时加载热歌榜。/ Load the hot chart on first entry or after a failure.
    if (st.list_count == 0 && !st.list_loading && st.list_source != MUSIC_SRC_SEARCH) music_load(st.list_source, NULL, st.list_offset);
    s_page = st.queue_index >= 0 && st.queue_source == st.list_source && st.queue_offset == st.list_offset
                 ? st.queue_index / ROWS : 0;
}

static void music_exit(app_ctx_t *ctx) {
    (void)ctx;
    music_page_open(false);
    if (s_view == VIEW_SEARCH) ui_keyboard_end();
    s_view = VIEW_LIST;
}

const app_desc_t app_music = {
    .title = "网易云音乐",
    .detail = "热歌榜、我喜欢、每日推荐与搜索",
    .binary_ui = true,
    .owns_keys = true,
    .on_enter = on_enter,
    .on_exit = music_exit,
    .render = render,
    .present = music_present,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
