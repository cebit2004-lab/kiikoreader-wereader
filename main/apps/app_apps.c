/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：底栏「应用」页：音乐、背单词、AI 助手三个入口。
 * English: The "Apps" tab: entries for music, vocabulary and the AI assistant.
 */
#include <stdio.h>

#include "app.h"
#include "app_registry.h"
#include "music_player.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_nav_layout.h"
#include "ui_root_style.h"

extern const app_desc_t app_music;
extern const app_desc_t app_vocab;
extern const app_desc_t app_ai;

// 从这里打开的子页返回时回到本页；从别处（如 AI 键）打开的回首页。
// Subpages opened from here return here; those opened elsewhere (e.g. the AI key) return home.
const app_desc_t *g_apps_origin;
extern const app_desc_t app_apps;

typedef struct {
    const char *title;
    const char *detail;
    ui_icon_t icon;
    const app_desc_t *page;
} apps_entry_t;

static const apps_entry_t k_entries[] = {
    {"网易云音乐", "热歌榜、我喜欢、每日推荐、搜索", UI_ICON_RADIO_TOWER, &app_music},
    {"背单词", "导入词库，按记忆曲线复习", UI_ICON_BOOK_BOOKMARK, &app_vocab},
    {"AI 助手", "按住侧面 AI 键说话", UI_ICON_ZAP, &app_ai},
};
#define ENTRY_COUNT ((int)(sizeof(k_entries) / sizeof(k_entries[0])))

static EpdRect entry_rect(int i) { return (EpdRect){36, 196 + i * 196, 612, 168}; }

static bool main_page_visible(app_ctx_t *ctx) { (void)ctx; return true; }

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_root_background(fb);
    ui_nav_status(fb);
    ui_text_box(fb, (EpdRect){36, 80, 612, 76}, 52, "应用", EPD_DRAW_ALIGN_LEFT, false);
    music_status_t music;
    music_get_status(&music);
    for (int i = 0; i < ENTRY_COUNT; ++i) {
        EpdRect r = entry_rect(i);
        ui_root_card(fb, r, 24);
        ui_draw_icon(fb, r.x + 62, r.y + r.height / 2, 52, k_entries[i].icon, 0x30);
        ui_text_box(fb, (EpdRect){r.x + 120, r.y + 30, 440, 56}, 40, k_entries[i].title, EPD_DRAW_ALIGN_LEFT, false);
        const char *detail = k_entries[i].detail;
        char playing[MUSIC_NAME_MAX + 32];
        if (k_entries[i].page == &app_music && music.state != MUSIC_STOPPED) {
            snprintf(playing, sizeof(playing), "%s：%s", music.state == MUSIC_PAUSED ? "已暂停" : "正在播放", music.title);
            detail = playing;
        }
        ui_text_box(fb, (EpdRect){r.x + 120, r.y + 96, 450, 40}, 24, detail, EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_icon(fb, r.x + r.width - 34, r.y + r.height / 2, 26, UI_ICON_CHEVRON_RIGHT, 0x30);
    }
    ui_nav_draw(fb, UI_TAB_APPS);
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    for (int i = 0; i < ENTRY_COUNT; ++i) {
        if (ui_rect_hit(entry_rect(i), ev->x0, ev->y0)) {
            g_apps_origin = &app_apps;
            ctx->request_app = k_entries[i].page;
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static unsigned s_seen_music;
static app_redraw_t on_tick(app_ctx_t *ctx) {
    (void)ctx;
    // 后台播放切歌时更新入口说明。/ Refresh the entry caption when background playback changes track.
    music_status_t music;
    music_get_status(&music);
    const unsigned sig = (unsigned)(music.queue_index + 1) * 4u + (unsigned)music.state;
    if (sig == s_seen_music) return APP_REDRAW_NONE;
    s_seen_music = sig;
    return APP_REDRAW_PAGE;
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    music_status_t music;
    music_get_status(&music);
    s_seen_music = (unsigned)(music.queue_index + 1) * 4u + (unsigned)music.state;
}

const app_desc_t app_apps = {
    .title = "应用",
    .detail = "音乐、背单词与 AI 助手",
    .binary_ui = true,
    .persistent_navigation = true,
    .main_page_visible = main_page_visible,
    .on_enter = on_enter,
    .render = render,
    .on_gesture = on_gesture,
    .on_tick = on_tick,
};
