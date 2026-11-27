/* SPDX-License-Identifier: Apache-2.0
 * 中文：顶部快捷菜单的独立手势和纯绘图；动作由主循环执行。
 * English: Independent gesture recognition and pure drawing for the top quick layer; the loop owns actions.
 * 用户修订：展开复用现有底页并生成液态玻璃；只保留顶部4bpp底图及四圆键，收起还原，点按局部反馈。
 * User revision: open over the existing page; retain only a packed top underlay and four keys, restore on close, and acknowledge presses locally.
 * 冻结：仅边缘起手；不做连续动画或整帧缓存；关闭、切页、睡眠和错误时释放控件缓存。
 * Frozen: edge-origin capture only, no continuous animation or full-frame cache; release key snapshots on close, navigation, sleep and errors.
 */
#pragma once
#include "app.h"

#define UI_QUICK_EDGE_PX 32
#define UI_QUICK_SWIPE_PX 72
#define UI_QUICK_HEIGHT 384
#define UI_QUICK_CENTER_X(i) (100 + ((i) % 2) * 144)
#define UI_QUICK_CENTER_Y(i) (152 + ((i) / 2) * 132)
#define UI_QUICK_RADIUS 48
#define UI_QUICK_HIT_RADIUS 58
#define UI_QUICK_FILL_DARK 0x44
#define UI_QUICK_SLIDER_X(warmth) ((warmth) ? 514 : 368)
#define UI_QUICK_SLIDER_WIDTH 116
#define UI_QUICK_SLIDER_Y0 104
#define UI_QUICK_SLIDER_Y1 344
#define UI_QUICK_SLIDER_RANGE (UI_QUICK_SLIDER_Y1-UI_QUICK_SLIDER_Y0)
#include "ui_quick_brightness.h"

typedef enum {
    UI_QUICK_NONE, UI_QUICK_OPEN, UI_QUICK_CLOSE,
    UI_QUICK_WIFI, UI_QUICK_BLUETOOTH, UI_QUICK_FULL, UI_QUICK_FRONTLIGHT,
    UI_QUICK_BRIGHTNESS, UI_QUICK_WARMTH, UI_QUICK_LIGHT_COMMIT,
} ui_quick_action_t;

typedef struct {
    bool open, tracking, swallow, moved;
    uint16_t x0, y0;
    int hit;
    int slider_value;
    int slider_position;
    int64_t started_ms;
} ui_quick_menu_t;

/// 清除层和手势；切页、睡眠、媒体失效时使用。/ Clear layer and contact state at navigation, sleep and media-loss boundaries.
void ui_quick_menu_reset(ui_quick_menu_t *menu);
/// 返回true表示整条触摸序列被接管；多指/读错不执行按钮，收起之后仍吞到抬手。
/// True owns the whole contact stream; multitouch/read errors never activate controls, and dismissal still swallows until release.
bool ui_quick_menu_feed(ui_quick_menu_t *menu, const app_ctx_t *ctx, bool allowed,
                       ui_quick_action_t *action);
/// 取消接触但保留已打开的层。/ Cancel a contact while retaining an already open layer.
void ui_quick_menu_cancel_input(ui_quick_menu_t *menu);
/// 只覆盖顶部，保留下层其他像素；44px图标与底栏相同，状态栏正常绘制。
/// Cover the top only, leaving other pixels intact; use the same 44px icon size as navigation and draw the normal status bar.
/// soften_text仅用于阅读正文：轻微柔化玻璃背后的文字，收起恢复原像素，点按不重复模糊。
/// soften_text is for reader body only: gently blur text behind glass, restore original pixels on close and never reblur on input.
void ui_quick_menu_draw(uint8_t *fb, bool wifi, bool bluetooth, bool frontlight, unsigned brightness, unsigned warmth,bool soften_text);
/// 独立圆钮的绘图与内部刷新范围；不重新合成下层。/ Draw one control without recomposing the underlay.
bool ui_quick_menu_draw_button(uint8_t *fb, unsigned index, bool enabled, bool pressed, EpdRect *dirty);
int ui_quick_menu_pressed(const ui_quick_menu_t *menu);
/// 精确还原顶部底图；缓存不可用时返回false，调用方重绘当前页。/ Restore the exact top underlay; false asks the caller to repaint the current page.
bool ui_quick_menu_restore(uint8_t *fb);
void ui_quick_menu_end(void);
void ui_quick_menu_draw_status(uint8_t *fb);
EpdRect ui_quick_button_rect(unsigned index);
/// 滑块以0..240像素定位，不按整数百分比分档。/ Slider positions use 0..240 pixels.
void ui_quick_menu_draw_slider(uint8_t *fb, bool warmth, unsigned value);
EpdRect ui_quick_slider_rect(bool warmth);
EpdRect ui_quick_slider_change_rect(bool warmth, unsigned before, unsigned after);
EpdRect ui_quick_menu_rect(void);
