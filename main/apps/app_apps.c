/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：底栏「应用」页：音乐、背单词、AI 助手三个入口。
 * English: The "Apps" tab: entries for music, vocabulary and the AI assistant.
 */
#include <stdio.h>

#include "app.h"
#include "ai_voice.h"
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

/*
 * 布局（2026-11-26 美化）：和设置页同一套样式，白底、深色描边圆角卡片、灰色分组小标题，快刷黑白下
 * 边界也清楚。上方一张宽卡片是网易云音乐，卡片里直接放四个来源入口；下方两张方卡片是背单词和 AI 助手。
 * Layout (polished 2026-11-26): the Settings page's style, white canvas, dark-framed rounded cards and
 * small group captions, so edges stay clear under black-and-white fast refresh. A wide music card on
 * top carries the four source entries directly; two square cards below hold vocabulary and the AI.
 */
#define MUSIC_CARD ((EpdRect){36, 206, 612, 268})
#define VOCAB_CARD ((EpdRect){36, 560, 300, 330})
#define AI_CARD ((EpdRect){348, 560, 300, 330})
#define CARD_RADIUS 22
#define CARD_EDGE 0x70

static const char *const k_sources[MUSIC_SRC_COUNT] = {"热歌榜", "我喜欢", "每日推荐", "搜索"};
static EpdRect source_rect(int i) { return (EpdRect){60 + i * 144, MUSIC_CARD.y + 172, 132, 68}; }

void app_music_open_source(int source);
void app_vocab_summary(char *line1, int size1, char *line2, int size2);

static bool main_page_visible(app_ctx_t *ctx) { (void)ctx; return true; }

static void card(uint8_t *fb, EpdRect r) {
    ui_fill_round_rect(fb, r, CARD_RADIUS, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, r, CARD_RADIUS, CARD_EDGE);
}

static void caption(uint8_t *fb, int y, const char *text) { ui_text(fb, 42, y, 20, text, EPD_DRAW_ALIGN_LEFT, false); }

// 圆形图标底：深色描边圆环里放图标。/ Icon badge: the icon inside a dark ring.
static void badge(uint8_t *fb, int cx, int cy, int r, ui_icon_t icon, int size) {
    const EpdRect ring = {cx - r, cy - r, 2 * r, 2 * r};
    ui_fill_round_rect(fb, ring, r, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, ring, r, CARD_EDGE);
    ui_draw_icon(fb, cx, cy, size, icon, UI_GRAY_BLACK);
}

static void render_music(uint8_t *fb) {
    const EpdRect r = MUSIC_CARD;
    card(fb, r);
    badge(fb, r.x + 66, r.y + 76, 40, UI_ICON_RADIO_TOWER, 44);
    ui_text_box(fb, (EpdRect){r.x + 126, r.y + 30, 440, 50}, 36, "网易云音乐", EPD_DRAW_ALIGN_LEFT, false);
    music_status_t st;
    music_get_status(&st);
    char line[MUSIC_NAME_MAX + MUSIC_ARTIST_MAX + 32];
    if (st.state != MUSIC_STOPPED && st.title[0])
        snprintf(line, sizeof(line), "%s：%s%s%s", st.state == MUSIC_PAUSED ? "已暂停" : "正在播放", st.title,
                 st.artist[0] ? " · " : "", st.artist);
    else
        snprintf(line, sizeof(line), "%s", "选一个歌单开始听");
    ui_text_box(fb, (EpdRect){r.x + 126, r.y + 84, 450, 40}, 23, line, EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_icon(fb, r.x + r.width - 34, r.y + 76, 26, UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
    for (int i = 0; i < MUSIC_SRC_COUNT; ++i) {
        const EpdRect chip = source_rect(i);
        // 正在播放的来源加粗显示。/ The source now playing gets the thick selected ring.
        if (st.state != MUSIC_STOPPED && st.queue_source == (music_source_t)i)
            ui_draw_selected_round_rect(fb, chip, chip.height / 2);
        else
            ui_draw_control_frame(fb, chip, chip.height / 2, CARD_EDGE);
        ui_text_box(fb, ui_inset_rect(chip, 8), 25, k_sources[i], EPD_DRAW_ALIGN_CENTER, false);
    }
}

static void render_small(uint8_t *fb, EpdRect r, ui_icon_t icon, const char *title, const char *line1, const char *line2) {
    card(fb, r);
    badge(fb, r.x + 70, r.y + 82, 44, icon, 48);
    ui_draw_icon(fb, r.x + r.width - 38, r.y + 82, 26, UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
    ui_text_box(fb, (EpdRect){r.x + 30, r.y + 150, r.width - 60, 52}, 36, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){r.x + 30, r.y + 214, r.width - 50, 36}, 22, line1, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){r.x + 30, r.y + 256, r.width - 50, 36}, 22, line2, EPD_DRAW_ALIGN_LEFT, false);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_text(fb, 36, 91, 52, "应用", EPD_DRAW_ALIGN_LEFT, false);
    caption(fb, 172, "音乐");
    render_music(fb);
    caption(fb, 526, "学习与助手");
    char vocab1[64], vocab2[64];
    app_vocab_summary(vocab1, sizeof(vocab1), vocab2, sizeof(vocab2));
    render_small(fb, VOCAB_CARD, UI_ICON_BOOK_BOOKMARK, "背单词", vocab1, vocab2);
    render_small(fb, AI_CARD, UI_ICON_ZAP, "AI 助手", ai_active() ? "正在对话" : "按住侧面 AI 键",
                 ai_active() ? "点开回到对话" : "或点这里开始说话");
    ui_nav_draw(fb, UI_TAB_APPS);
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    const app_desc_t *page = NULL;
    for (int i = 0; i < MUSIC_SRC_COUNT && !page; ++i) {
        if (ui_rect_hit(ui_inset_rect(source_rect(i), -6), ev->x0, ev->y0)) {
            app_music_open_source(i);
            page = &app_music;
        }
    }
    if (!page && ui_rect_hit(MUSIC_CARD, ev->x0, ev->y0)) page = &app_music;
    if (!page && ui_rect_hit(VOCAB_CARD, ev->x0, ev->y0)) page = &app_vocab;
    if (!page && ui_rect_hit(AI_CARD, ev->x0, ev->y0)) page = &app_ai;
    if (page) {
        g_apps_origin = &app_apps;
        ctx->request_app = page;
    }
    return APP_REDRAW_NONE;
}

static unsigned s_seen_music;
static app_redraw_t on_tick(app_ctx_t *ctx) {
    (void)ctx;
    // 后台播放切歌时更新入口说明。/ Refresh the entry caption when background playback changes track.
    music_status_t music;
    music_get_status(&music);
    const unsigned sig = (unsigned)(music.queue_index + 1) * 8u + (unsigned)music.state * 2u + (ai_active() ? 1u : 0u);
    if (sig == s_seen_music) return APP_REDRAW_NONE;
    s_seen_music = sig;
    return APP_REDRAW_PAGE;
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    music_status_t music;
    music_get_status(&music);
    s_seen_music = (unsigned)(music.queue_index + 1) * 8u + (unsigned)music.state * 2u + (ai_active() ? 1u : 0u);
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
