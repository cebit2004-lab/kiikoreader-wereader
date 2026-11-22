/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：AI 助手页。显示对话状态、你说的话和 AI 的回答（自动折行，过长时显示末尾）。对话本身在 ai_voice
 * 后台进行，离开本页继续；AI 键（短按功能设为「AI 助手」时）在任何页面都能开始、打断或结束。
 * 流式回答最多每 1.5 秒重绘一次，照顾墨水屏。
 *
 * English: AI assistant page. Shows the chat state, your words and the AI's reply (wrapped, showing the
 * tail when long). The chat runs in ai_voice and continues after leaving; the AI key (with its short
 * action set to "AI assistant") starts, interrupts or ends it from any page. A streaming reply
 * redraws at most every 1.5 s, sparing the e-paper.
 */
#include <stdio.h>
#include <string.h>

#include "ai_voice.h"
#include "app.h"
#include "app_registry.h"
#include "metalio_audio.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"

#define REDRAW_MIN_MS 1500
#define USER_TOP 230
#define REPLY_TOP 420
#define REPLY_BOTTOM 1010
#define MAX_LINES 48


// 返回：从「应用」页进来的回「应用」，否则回首页（不进总菜单）。
// Back: to the Apps page when opened from it, otherwise home (never the root menu).
extern const app_desc_t *g_apps_origin;
static void go_back(app_ctx_t *ctx) {
    ctx->request_app = g_apps_origin ? g_apps_origin : app_home_page();
    g_apps_origin = NULL;
}

static bool s_redraw_granted;  // 刚按语音停顿放行过一次刷屏 / a redraw was just granted at a pause in speech
static unsigned s_drawn_rev;
static ai_state_t s_drawn_state;
static int64_t s_drawn_ms;

static EpdRect main_button(void) { return (EpdRect){36, 1060, 400, 104}; }
static EpdRect side_button(void) { return (EpdRect){452, 1060, 196, 104}; }

static const char *state_text(ai_state_t st) {
    switch (st) {
    case AI_CONNECTING: return "正在连接…";
    case AI_LISTENING: return "请说话";
    case AI_HEARING: return "正在听…";
    case AI_THINKING: return "思考中…";
    case AI_SPEAKING: return "正在回答";
    default: return "未开始";
    }
}

// 按宽度把 UTF-8 文本折成多行，返回行数；starts/lens 记录每行起点与长度。px 须是实际绘制字号
// （ui_text 会按字号设置放大，量宽必须用同一字号，否则右侧溢出）。
// Wrap UTF-8 text to a width; returns the line count, with each line's start and length. px must be
// the size actually drawn (ui_text scales by the font-size setting; measuring at another size overflows
// the right edge).
static int wrap_lines(const char *text, int px, int width, int *starts, int *lens, int max) {
    int count = 0;
    const int len = (int)strlen(text);
    int pos = 0;
    char line[512];
    while (pos < len && count < max) {
        if (text[pos] == '\n') { ++pos; continue; }
        int end = pos, fit = pos;
        while (end < len && text[end] != '\n') {
            int next = end + 1;
            while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) ++next;  // 一个完整字符 / one whole character
            const int n = next - pos;
            if (n >= (int)sizeof(line)) break;
            memcpy(line, text + pos, (size_t)n);
            line[n] = 0;
            if (ui_text_fixed_width_px(px, line) > width) break;
            fit = end = next;
        }
        if (fit == pos) {  // 单个字符就超宽，强制放一个 / a single character overflows; take it anyway
            int next = pos + 1;
            while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) ++next;
            fit = next;
        }
        starts[count] = pos;
        lens[count] = fit - pos;
        ++count;
        pos = fit;
    }
    return count;
}

// 在区域内画折行文字；放不下且 tail 为真时显示末尾。/ Draw wrapped text in a box; on overflow, tail shows the end.
static void draw_wrapped(uint8_t *fb, EpdRect box, int px, const char *text, bool tail) {
    static int starts[MAX_LINES], lens[MAX_LINES];
    const int drawn_px = ui_text_effective_px(px);  // ui_text 实际使用的字号 / the size ui_text really draws at
    const int count = wrap_lines(text, drawn_px, box.width, starts, lens, MAX_LINES);
    const int line_h = drawn_px * 3 / 2;
    const int fit = box.height / line_h;
    const int first = tail && count > fit ? count - fit : 0;
    char line[512];
    for (int i = first, row = 0; i < count && row < fit; ++i, ++row) {
        const int n = lens[i] < (int)sizeof(line) - 1 ? lens[i] : (int)sizeof(line) - 1;
        memcpy(line, text + starts[i], (size_t)n);
        line[n] = 0;
        ui_text(fb, box.x, box.y + row * line_h, px, line, EPD_DRAW_ALIGN_LEFT, false);
    }
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    ai_status_t st;
    ai_get_status(&st);
    s_drawn_rev = st.revision;
    s_drawn_state = st.state;
    s_drawn_ms = ctx->now_ms;

    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "AI 助手", EPD_DRAW_ALIGN_CENTER, false);

    // 状态。/ State.
    const bool active = st.state != AI_IDLE;
    EpdRect pill = {36, 160, 612, 52};
    if (active) ui_fill_round_rect(fb, pill, 26, 0x30);
    else ui_draw_round_rect(fb, pill, 26, 0x90);
    char head[96];
    if (active && st.mic_peak > 0 && (st.state == AI_LISTENING || st.state == AI_HEARING))
        snprintf(head, sizeof(head), "%s · 音量 %d", state_text(st.state), st.mic_peak);
    else snprintf(head, sizeof(head), "%s", state_text(st.state));
    ui_text_box(fb, ui_inset_rect(pill, 8), 26, head, EPD_DRAW_ALIGN_CENTER, active);

    // 你说的话。/ Your words.
    ui_text(fb, 36, USER_TOP, 24, "你说", EPD_DRAW_ALIGN_LEFT, false);
    draw_wrapped(fb, (EpdRect){36, USER_TOP + 40, 612, 140}, 28,
                 st.user[0] ? st.user : (active ? "…" : "短按 AI 键或点下方按钮开始，直接说话即可"), true);
    ui_hairline(fb, REPLY_TOP - 16, 36, 612, 0xc0);

    // AI 回答。/ The reply.
    ui_text(fb, 36, REPLY_TOP, 24, "AI", EPD_DRAW_ALIGN_LEFT, false);
    if (st.error[0]) draw_wrapped(fb, (EpdRect){36, REPLY_TOP + 40, 612, 180}, 28, st.error, false);
    else draw_wrapped(fb, (EpdRect){36, REPLY_TOP + 40, 612, REPLY_BOTTOM - REPLY_TOP - 40}, 30, st.reply, true);

    // 按钮。/ Buttons.
    if (!active) {
        ui_draw_button(fb, (EpdRect){36, 1060, 612, 104}, "开始对话", true);
    } else {
        ui_draw_button(fb, main_button(), st.state == AI_SPEAKING ? "打断" : st.state == AI_CONNECTING ? "取消" : "结束对话", true);
        ui_draw_button(fb, side_button(), "结束", false);
    }
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const uint16_t x = ev->x0, y = ev->y0;
    // 返回区覆盖左上角一大块，墨水屏上手指常点偏。/ A generous back area: fingers often land off the small icon.
    if (y < 200 && x < 160) { go_back(ctx); return APP_REDRAW_NONE; }
    if (!ai_active()) {
        if (ui_rect_hit((EpdRect){36, 1060, 612, 104}, x, y)) ai_start();
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(main_button(), x, y)) { ai_toggle(); return APP_REDRAW_NONE; }
    if (ui_rect_hit(side_button(), x, y)) { ai_stop(); return APP_REDRAW_NONE; }
    return APP_REDRAW_NONE;
}

// 接管按键：音量键调音量（AI 说话时也能调），中键返回，中键长按全刷。
// Own the keys: volume keys set the volume (also while the AI speaks), the middle key returns, a hold refreshes.
static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (key == UI_KEY_1 || key == UI_KEY_3) {
        // 实机上 UI_KEY_1 对应音量+。/ On the device UI_KEY_1 is volume up.
        metalio_audio_set_volume(metalio_audio_volume() + (key == UI_KEY_1 ? 10 : -10));
        return APP_REDRAW_NONE;
    }
    go_back(ctx);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    (void)ctx;
    return key == UI_KEY_2 ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    // 上一次放行的刷屏已经同步推完屏，让播放继续。/ The granted redraw has been pushed synchronously; let playback resume.
    if (s_redraw_granted) {
        s_redraw_granted = false;
        ai_redraw_done();
    }
    ai_status_t st;
    ai_get_status(&st);
    if (st.revision == s_drawn_rev) return APP_REDRAW_NONE;
    // 状态变了立即重绘，文字增长限速。/ Redraw at once on a state change; throttle text growth.
    if (st.state == s_drawn_state && ctx->now_ms - s_drawn_ms < REDRAW_MIN_MS) return APP_REDRAW_NONE;
    // 对话中每次刷屏都先向播放要时机：墨水屏刷新时驱动在两个核上跑最高优先级送数任务，播放供不上 I2S
    // （DMA 只有 30 ms 余量），落在发声处就破音，落在停顿里只是停顿稍长。状态刷新也一样：变成“正在
    // 回答”的那次刷新与起播前的 1 秒预缓冲同时进行，首次回答字形缓存是冷的，排版加推屏可能超过 1 秒，
    // 一直刷到开口之后。不出声时播放随即放行（最多 20 ms），并等刷完才开始出声。
    // During a chat every redraw first asks playback for a slot: an e-paper refresh runs top-priority
    // feed tasks on both cores and playback cannot feed I2S (30 ms of DMA), so a refresh during sound
    // crackles while one in a pause just lengthens it. State changes too: the redraw into "speaking"
    // runs alongside the 1 s pre-playback buffer, and on the first reply the glyph cache is cold, so
    // layout plus push can run past 1 s, into the sound. While silent, playback grants at once (within
    // 20 ms) and does not start sounding until the refresh is done.
    if (!ai_redraw_slot()) return APP_REDRAW_NONE;
    s_redraw_granted = true;  // 下一次 tick 通知播放继续 / tell playback to resume on the next tick
    return APP_REDRAW_PAGE;
}

static void on_enter(app_ctx_t *ctx) { (void)ctx; s_drawn_rev = 0; }

// 离开页面就结束对话，不在后台占着网络和麦克风。/ Leaving the page ends the chat; nothing lingers on the network or mic.
static void ai_exit(app_ctx_t *ctx) {
    (void)ctx;
    if (s_redraw_granted) {  // 别让播放白等 / do not leave playback waiting
        s_redraw_granted = false;
        ai_redraw_done();
    }
    ai_stop();
}

const app_desc_t app_ai = {
    .title = "AI 助手",
    .detail = "语音对话",
    .binary_ui = true,
    .on_enter = on_enter,
    .on_exit = ai_exit,
    .render = render,
    .on_gesture = on_gesture,
    .owns_keys = true,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .on_tick = on_tick,
};
