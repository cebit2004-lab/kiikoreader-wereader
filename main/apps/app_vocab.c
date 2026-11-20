/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：背单词页（占位，功能开发中）。/ English: vocab page (placeholder while in development).
 */
#include "app.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "背单词", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_box(fb, (EpdRect){52, 500, 580, 60}, 30, "词库导入与复习，正在开发", EPD_DRAW_ALIGN_CENTER, false);
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_TAP && ev->y0 < 160 && ev->x0 < 120) ctx->request_return = true;
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    (void)key;
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

const app_desc_t app_vocab = {
    .title = "背单词",
    .detail = "词库导入与复习，正在开发",
    .binary_ui = true,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
};
