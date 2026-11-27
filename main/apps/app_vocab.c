/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：背单词页。内置人教版小学（三至六年级，按册、单元）、初中中考词汇、常用 2000 词。
 *   首页：当前词表卡片（进度与「开始学习」）、出题方式（混合 / 看英文选中文 / 看中文选英文）、
 *   错词本与防遗忘复习入口，下面按阶段选范围——整个阶段、某个年级、某一册或某个单元。
 *   答题：一选四。选完不论对错都进讲解页：音标、词性释义、词形变化（复数、过去式、比较级、宾格、
 *   派生的形容词/副词/名词等）和中英例句。答错或「不知道」的词进错词本，本轮稍后再出一次；
 *   错词连续答对 3 次自动移出。防遗忘：全部词库里按记忆曲线今天到期的词。中键返回，长按中键全刷。
 * English: Vocabulary page. Built-in PEP primary (grades 3-6, by term and unit), junior-high
 *   zhongkao words and the 2000 most common words. Home holds the current list card, the quiz
 *   direction, the wrong-word book and anti-forgetting entries, and scope pickers by stage.
 *   Questions are four-choice; every answer opens an explanation with phonetics, POS and meaning,
 *   word forms and an English–Chinese example. Missed words go to the wrong-word book and return
 *   later in the round; three right answers in a row clear them. Anti-forgetting reviews every word
 *   due today. The middle key goes back; holding it forces a full refresh.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_registry.h"
#include "esp_random.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "vocab_book.h"

typedef enum { VIEW_HOME, VIEW_BOOK, VIEW_QUIZ, VIEW_RESULT, VIEW_DONE, VIEW_WRONG, VIEW_DETAIL } view_t;

#define W 612
#define X0 36
// 首页 / Home.
#define CARD ((EpdRect){X0, 166, W, 236})
#define START_BTN ((EpdRect){X0 + 24, 166 + 146, W - 48, 72})
#define DIR_TOP 418
#define TOOL_TOP 496
#define TAB_TOP 626
#define TAB_H 64
#define PICK_TOP 702
#define STAGE_ROW_H 76
#define GRADE_ROW_H 100
// 列表页 / List pages.
#define BOOK_TOP 176
#define LIST_ROW_H 82
#define LIST_ROWS 10
#define LIST_PAGER 1040
#define WRONG_TOP 276
#define WRONG_ROW_H 76
// 答题与讲解 / Quiz and explanation.
#define OPTION_TOP 470
#define OPTION_H 104
#define OPTION_GAP 120
#define IDK_BTN ((EpdRect){X0, 970, W, 84})
#define NEXT_BTN ((EpdRect){X0, 1092, W, 96})
#define LEFT_BTN ((EpdRect){X0, 1092, 300, 96})
#define RIGHT_BTN ((EpdRect){X0 + 312, 1092, 300, 96})

extern const app_desc_t *g_apps_origin;
static view_t s_view;
static int s_stage, s_book, s_page;
// 当前题目：选项、正确项、所选项、本题方向（混合模式下每题随机）。/ Current question state.
static int s_options[4], s_right, s_picked;
static bool s_zh_en;
static int s_shown_id;              // 讲解页的词 / word on the explanation page
static uint16_t s_wrong_ids[512];
static int s_wrong_count;

static void go_back(app_ctx_t *ctx) {
    vocab_save();
    ctx->request_app = g_apps_origin ? g_apps_origin : app_home_page();
    g_apps_origin = NULL;
}

static EpdRect tab_rect(int i) { return (EpdRect){X0 + i * (W / 3), TAB_TOP, W / 3, TAB_H}; }
static EpdRect dir_rect(int i) { return (EpdRect){X0 + i * 208, DIR_TOP, 196, 64}; }
static EpdRect tool_rect(int i) { return (EpdRect){X0 + i * 208, TOOL_TOP, 196, 84}; }
#define DAILY_CHIP ((EpdRect){X0 + W - 214, 166 + 8, 196, 40})
static EpdRect grade_rect(int row) { return (EpdRect){X0, PICK_TOP + STAGE_ROW_H + row * GRADE_ROW_H, W, GRADE_ROW_H}; }
static EpdRect term_chip(int row, int term) {
    const EpdRect r = grade_rect(row);
    return (EpdRect){r.x + 300 + term * 156, r.y + 16, 144, 68};
}
static EpdRect option_rect(int i) { return (EpdRect){X0, OPTION_TOP + i * OPTION_GAP, W, OPTION_H}; }

// 小学阶段的册两两成一个年级；其他阶段每个阶段一册。/ Primary books pair into grades; other stages hold one book.
static bool stage_has_grades(int stage) { return vocab_stage_books(stage).count > 2; }

static void header(uint8_t *fb, const char *title) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
}

static void frame(uint8_t *fb, EpdRect r, int radius) {
    ui_fill_round_rect(fb, r, radius, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, r, radius, 0x70);
}

static void chip(uint8_t *fb, EpdRect r, const char *label, bool on, int px) {
    if (on) ui_fill_round_rect(fb, r, r.height / 2, 0x30);
    else ui_draw_control_frame(fb, r, r.height / 2, 0x70);
    ui_text_box(fb, ui_inset_rect(r, 10), px, label, EPD_DRAW_ALIGN_CENTER, on);
}

static void scope_line(char *out, int size, vocab_scope_t sc) {
    vocab_stats_t st;
    vocab_scope_stats(sc, &st);
    snprintf(out, size, "%d 词 · 已学 %d · 掌握 %d", st.total, st.learned, st.mastered);
}

// 自动换行：英文按词断，中文按字断；超出行数时末行加省略号。返回下一行的 y。
// Wrap text: English breaks between words, Chinese between characters; an overflowing last line ends
// with an ellipsis. Returns the y below the text.
static int wrap(uint8_t *fb, int x, int y, int w, int px, const char *text, int max_lines) {
    const int line_h = px * 4 / 3 + 4;
    char line[192] = "";
    int lines = 0;
    const char *p = text;
    while (*p && lines < max_lines) {
        // 取下一个单位：一个英文词连同其后空格，或一个多字节字符。/ Next unit: a word plus spaces, or one multibyte char.
        const char *q = p;
        if ((unsigned char)*q < 0x80) {
            while (*q && (unsigned char)*q < 0x80 && *q != ' ') ++q;
            while (*q == ' ') ++q;
            if (q == p) ++q;
        } else {
            ++q;
            while ((*q & 0xc0) == 0x80) ++q;
        }
        char trial[192];
        snprintf(trial, sizeof(trial), "%s%.*s", line, (int)(q - p), p);
        if (line[0] && ui_text_fixed_width_px(px, trial) > w) {
            if (lines == max_lines - 1) {
                // 先量好再加省略号：放不下就从行尾退掉整字。/ Measure first: drop whole characters from the
                // end until the line plus the ellipsis fits.
                size_t len = strlen(line);
                for (;;) {
                    while (len && line[len - 1] == ' ') line[--len] = 0;
                    snprintf(trial, sizeof(trial), "%s…", line);
                    if (!len || ui_text_fixed_width_px(px, trial) <= w) break;
                    do --len; while (len && ((unsigned char)line[len] & 0xc0) == 0x80);
                    line[len] = 0;
                }
                snprintf(line, sizeof(line), "%s", trial);
                p = "";
                break;
            }
            ui_text_fixed(fb, x, y, px, line, EPD_DRAW_ALIGN_LEFT, false);
            y += line_h;
            ++lines;
            line[0] = 0;
            continue;
        }
        snprintf(line, sizeof(line), "%s", trial);
        p = q;
    }
    if (line[0] && lines < max_lines) { ui_text_fixed(fb, x, y, px, line, EPD_DRAW_ALIGN_LEFT, false); y += line_h; }
    return y;
}

// ---------------------------------------------------------------- home

static void render_home(uint8_t *fb) {
    header(fb, "背单词");
    const EpdRect c = CARD;
    frame(fb, c, 22);
    char name[64], line[96];
    const vocab_scope_t sc = vocab_scope();
    vocab_scope_name(sc, name, sizeof(name));
    vocab_stats_t st;
    vocab_scope_stats(sc, &st);
    // 今日进度与打卡；右上角点一下切换每日新词量。/ Today's progress and streak; the top-right chip cycles the quota.
    const int streak = vocab_streak();
    if (streak) snprintf(line, sizeof(line), "今日新词 %d/%d · 连续打卡 %d 天", vocab_new_today(), vocab_daily_new(), streak);
    else snprintf(line, sizeof(line), "今日新词 %d/%d · 今天还没打卡", vocab_new_today(), vocab_daily_new());
    ui_text_box(fb, (EpdRect){c.x + 28, c.y + 12, 360, 34}, 21, line, EPD_DRAW_ALIGN_LEFT, false);
    char quota_label[32];
    snprintf(quota_label, sizeof(quota_label), "每天 %d 个新词", vocab_daily_new());
    chip(fb, DAILY_CHIP, quota_label, false, 20);
    ui_text_box(fb, (EpdRect){c.x + 28, c.y + 52, c.width - 56, 44}, 34, name, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "共 %d 词 · 已学 %d · 掌握 %d · 待复习 %d", st.total, st.learned, st.mastered, st.due);
    ui_text_box(fb, (EpdRect){c.x + 28, c.y + 100, c.width - 56, 34}, 22, line, EPD_DRAW_ALIGN_LEFT, false);
    const int quota = vocab_daily_new() > vocab_new_today() ? vocab_daily_new() - vocab_new_today() : 0;
    int fresh = st.fresh < VOCAB_NEW_PER_ROUND ? st.fresh : VOCAB_NEW_PER_ROUND;
    if (fresh > quota) fresh = quota;
    if (st.due + fresh == 0) snprintf(line, sizeof(line), st.fresh && !quota ? "今天的新词学完了，明天继续" : "这个词表今天都学完了");
    else if (st.due) snprintf(line, sizeof(line), "开始学习（复习 %d · 新词 %d）", st.due, fresh);
    else snprintf(line, sizeof(line), "开始学习（新词 %d）", fresh);
    ui_draw_button(fb, START_BTN, line, st.due + fresh > 0);

    static const char *const k_dirs[3] = {"混合出题", "看英文选中文", "看中文选英文"};
    for (int i = 0; i < 3; ++i) chip(fb, dir_rect(i), k_dirs[i], vocab_dir() == (vocab_dir_t)i, 22);

    // 三个复习入口：当天回顾、错词本、防遗忘。/ Three review entries: same day, wrong words, anti-forgetting.
    const int wrong = vocab_wrong_list(NULL, 0), due_all = vocab_forget_due();
    vocab_today_t td;
    vocab_today_status(&td);
    static const char *const k_tools[3] = {"当天回顾", "错词本", "防遗忘"};
    for (int i = 0; i < 3; ++i) {
        const EpdRect r = tool_rect(i);
        // 当天回顾到点了就加粗提示。/ Highlight the same-day review when it is due.
        const bool ready = i == 0 && td.count && ((td.done == 0 && !td.wait_min) || (td.done == 1 && td.evening));
        if (ready) ui_draw_selected_round_rect(fb, r, 16);
        else frame(fb, r, 16);
        ui_text_box(fb, (EpdRect){r.x + 8, r.y + 8, r.width - 16, 40}, 28, k_tools[i], EPD_DRAW_ALIGN_CENTER, false);
        if (i == 0) {
            if (!td.count) snprintf(line, sizeof(line), "今天还没学新词");
            else if (td.done >= 2) snprintf(line, sizeof(line), "今天回顾完了");
            else if (td.done == 0 && td.wait_min) snprintf(line, sizeof(line), "%d 分钟后回顾", td.wait_min);
            else if (td.done == 0) snprintf(line, sizeof(line), "现在回顾 %d 词", td.count);
            else if (!td.evening) snprintf(line, sizeof(line), "晚上 7 点再来");
            else snprintf(line, sizeof(line), "睡前回顾 %d 词", td.count);
        } else if (i == 1) {
            if (wrong) snprintf(line, sizeof(line), "%d 个待巩固", wrong);
            else snprintf(line, sizeof(line), "还没有错词");
        } else {
            if (due_all) snprintf(line, sizeof(line), "今天到期 %d 词", due_all);
            else snprintf(line, sizeof(line), "今天没有到期");
        }
        ui_text_box(fb, (EpdRect){r.x + 8, r.y + 50, r.width - 16, 26}, 19, line, EPD_DRAW_ALIGN_CENTER, false);
    }

    ui_text(fb, 42, TAB_TOP - 32, 20, "选择词表", EPD_DRAW_ALIGN_LEFT, false);
    for (int i = 0; i < vocab_stage_count() && i < 3; ++i) {
        const EpdRect r = ui_inset_rect(tab_rect(i), 4);
        const bool on = i == s_stage;
        if (on) ui_fill_round_rect(fb, r, 14, 0x30);
        else ui_draw_round_rect(fb, r, 14, 0x90);
        ui_text_box(fb, ui_inset_rect(r, 8), 28, vocab_stage_name(i), EPD_DRAW_ALIGN_CENTER, on);
    }

    const vocab_scope_t books = vocab_stage_books(s_stage);
    const vocab_scope_t first = vocab_book_lists(books.first), last = vocab_book_lists(books.first + books.count - 1);
    const vocab_scope_t whole = {first.first, last.first + last.count - first.first};
    const EpdRect row = {X0, PICK_TOP, W, STAGE_ROW_H};
    char title[64];
    if (stage_has_grades(s_stage)) snprintf(title, sizeof(title), "整个%s阶段", vocab_stage_name(s_stage));
    else snprintf(title, sizeof(title), "%s · 全部", vocab_book_name(books.first));
    ui_text_box(fb, (EpdRect){row.x + 12, row.y + 6, 400, 38}, 28, title, EPD_DRAW_ALIGN_LEFT, false);
    scope_line(line, sizeof(line), whole);
    ui_text_box(fb, (EpdRect){row.x + 12, row.y + 44, 420, 26}, 19, line, EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, row.y + row.height - 1, row.x, row.width, 0xc0);
    if (!stage_has_grades(s_stage)) {
        // 单册阶段：第二行进到分组列表。/ Single-book stage: a second row opens its groups.
        const EpdRect g = grade_rect(0);
        snprintf(title, sizeof(title), "按组学习（%d 组）", vocab_book_lists(books.first).count);
        ui_text_box(fb, (EpdRect){g.x + 12, g.y + 14, 420, 40}, 28, title, EPD_DRAW_ALIGN_LEFT, false);
        ui_text_box(fb, (EpdRect){g.x + 12, g.y + 58, 420, 28}, 19, "每组 50 至 100 词，可以一组一组来",
                    EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_icon(fb, g.x + g.width - 24, g.y + g.height / 2, 24, UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
        return;
    }
    // 年级行：左边点年级学全年，右边两个册入口。/ Grade rows: the left side picks the whole year, chips open a term.
    for (int g = 0; g * 2 < books.count; ++g) {
        const EpdRect r = grade_rect(g);
        const int b = books.first + g * 2;
        const vocab_scope_t up = vocab_book_lists(b), down = vocab_book_lists(b + 1 < books.first + books.count ? b + 1 : b);
        snprintf(title, sizeof(title), "%.*s", (int)strlen("三年级"), vocab_book_name(b));
        ui_text_box(fb, (EpdRect){r.x + 12, r.y + 12, 260, 40}, 30, title, EPD_DRAW_ALIGN_LEFT, false);
        vocab_stats_t gs;
        vocab_scope_stats((vocab_scope_t){up.first, down.first + down.count - up.first}, &gs);
        snprintf(line, sizeof(line), "全年 %d 词 · 已学 %d", gs.total, gs.learned);
        ui_text_box(fb, (EpdRect){r.x + 12, r.y + 58, 280, 26}, 19, line, EPD_DRAW_ALIGN_LEFT, false);
        // 没有下册就不画它。/ No lower-term chip when that book does not exist.
        for (int t = 0; t < 2 && b + t < books.first + books.count; ++t) chip(fb, term_chip(g, t), t ? "下册" : "上册", false, 26);
        ui_hairline(fb, r.y + r.height - 1, r.x, r.width, 0xc0);
    }
}

static void open_book(int book) {
    s_book = book;
    s_page = 0;
    s_view = VIEW_BOOK;
}

static void next_question(void);

static app_redraw_t start_round(vocab_round_kind_t kind) {
    if (vocab_round_start(kind) == 0) return APP_REDRAW_NONE;
    next_question();
    return APP_REDRAW_PAGE;
}

static void open_wrong(void) {
    const int cap = (int)(sizeof(s_wrong_ids) / sizeof(s_wrong_ids[0]));
    s_wrong_count = vocab_wrong_list(s_wrong_ids, cap);
    if (s_wrong_count > cap) s_wrong_count = cap;
    s_page = 0;
    s_view = VIEW_WRONG;
}

static app_redraw_t home_tap(int x, int y) {
    if (ui_rect_hit(START_BTN, x, y)) return start_round(VOCAB_ROUND_SCOPE);
    if (ui_rect_hit(ui_inset_rect(DAILY_CHIP, -6), x, y)) { vocab_cycle_daily_new(); return APP_REDRAW_PAGE; }
    for (int i = 0; i < 3; ++i)
        if (ui_rect_hit(dir_rect(i), x, y)) {
            if (vocab_dir() == (vocab_dir_t)i) return APP_REDRAW_NONE;
            vocab_set_dir((vocab_dir_t)i);
            return APP_REDRAW_PAGE;
        }
    if (ui_rect_hit(tool_rect(0), x, y)) return start_round(VOCAB_ROUND_TODAY);
    if (ui_rect_hit(tool_rect(1), x, y)) { open_wrong(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(tool_rect(2), x, y)) return start_round(VOCAB_ROUND_FORGET);
    for (int i = 0; i < vocab_stage_count() && i < 3; ++i)
        if (ui_rect_hit(tab_rect(i), x, y)) {
            if (i == s_stage) return APP_REDRAW_NONE;
            s_stage = i;
            return APP_REDRAW_PAGE;
        }
    const vocab_scope_t books = vocab_stage_books(s_stage);
    if (y >= PICK_TOP && y < PICK_TOP + STAGE_ROW_H) {
        const vocab_scope_t first = vocab_book_lists(books.first), last = vocab_book_lists(books.first + books.count - 1);
        vocab_set_scope((vocab_scope_t){first.first, last.first + last.count - first.first});
        return APP_REDRAW_PAGE;
    }
    if (!stage_has_grades(s_stage)) {
        if (ui_rect_hit(grade_rect(0), x, y)) { open_book(books.first); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    for (int g = 0; g * 2 < books.count; ++g) {
        if (!ui_rect_hit(grade_rect(g), x, y)) continue;
        const int b = books.first + g * 2;
        for (int t = 0; t < 2; ++t)
            if (b + t < books.first + books.count && ui_rect_hit(ui_inset_rect(term_chip(g, t), -8), x, y)) {
                open_book(b + t);
                return APP_REDRAW_PAGE;
            }
        // 和绘制一样收住：没有下册时全年就是上册。/ Clamped like the render: without a lower term the year is one book.
        const vocab_scope_t up = vocab_book_lists(b), down = vocab_book_lists(b + 1 < books.first + books.count ? b + 1 : b);
        vocab_set_scope((vocab_scope_t){up.first, down.first + down.count - up.first});
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

// ---------------------------------------------------------------- book (units / groups)

static int book_rows(void) { return vocab_book_lists(s_book).count + 1; }   // 首行是整册 / first row is the whole book
static int book_pages(void) { return (book_rows() + LIST_ROWS - 1) / LIST_ROWS; }

static void render_book(uint8_t *fb) {
    header(fb, vocab_book_name(s_book));
    const vocab_scope_t lists = vocab_book_lists(s_book);
    const vocab_scope_t cur = vocab_scope();
    char title[96], line[96];
    for (int row = 0; row < LIST_ROWS; ++row) {
        const int index = s_page * LIST_ROWS + row;
        if (index >= book_rows()) break;
        const EpdRect r = {X0, BOOK_TOP + row * LIST_ROW_H, W, LIST_ROW_H};
        const vocab_scope_t sc = index == 0 ? lists : (vocab_scope_t){lists.first + index - 1, 1};
        if (sc.first == cur.first && sc.count == cur.count)
            ui_fill_round_rect(fb, (EpdRect){r.x, r.y + 4, r.width, r.height - 8}, 10, 0xe0);
        vocab_word_t a, b;
        if (index == 0) snprintf(title, sizeof(title), "整册学习");
        // 组的首尾词提示内容。/ First and last word hint what a group holds.
        else if (!stage_has_grades(s_stage) && vocab_list_size(sc.first) > 1 &&
                 vocab_word(vocab_list_word(sc.first, 0), &a) &&
                 vocab_word(vocab_list_word(sc.first, vocab_list_size(sc.first) - 1), &b))
            snprintf(title, sizeof(title), "%s  %s – %s", vocab_list_name(sc.first), a.en, b.en);
        else snprintf(title, sizeof(title), "%s", vocab_list_name(sc.first));
        ui_text_box(fb, (EpdRect){r.x + 14, r.y + 8, 540, 40}, 28, title, EPD_DRAW_ALIGN_LEFT, false);
        scope_line(line, sizeof(line), sc);
        ui_text_box(fb, (EpdRect){r.x + 14, r.y + 48, 520, 28}, 19, line, EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_icon(fb, r.x + r.width - 24, r.y + r.height / 2, 22, UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
        ui_hairline(fb, r.y + r.height - 1, r.x, r.width, 0xc0);
    }
    if (book_pages() > 1) {
        snprintf(line, sizeof(line), "%d/%d", s_page + 1, book_pages());
        ui_pager_draw_at(fb, line, s_page, book_pages(), false, -1, LIST_PAGER);
    }
    ui_text_box(fb, (EpdRect){X0, LIST_PAGER + 96, W, 32}, 20, "点一行设为当前词表", EPD_DRAW_ALIGN_CENTER, false);
}

// 分页条点击；没点中返回 APP_REDRAW_DONE。/ Pager tap; APP_REDRAW_DONE when it missed.
static app_redraw_t pager_tap(int x, int y, int pages) {
    if (pages < 2) return APP_REDRAW_DONE;
    const int pager = ui_pager_hit_at(x, y, LIST_PAGER);
    if (pager == UI_PAGER_NONE) return APP_REDRAW_DONE;
    const int target = ui_pager_target(pager, s_page, pages);
    if (target == s_page) return APP_REDRAW_NONE;
    s_page = target;
    return APP_REDRAW_PAGE;
}

static app_redraw_t book_tap(int x, int y) {
    const app_redraw_t paged = pager_tap(x, y, book_pages());
    if (paged != APP_REDRAW_DONE) return paged;
    if (y < BOOK_TOP || y >= BOOK_TOP + LIST_ROWS * LIST_ROW_H) return APP_REDRAW_NONE;
    const int index = s_page * LIST_ROWS + (y - BOOK_TOP) / LIST_ROW_H;
    if (index >= book_rows()) return APP_REDRAW_NONE;
    const vocab_scope_t lists = vocab_book_lists(s_book);
    vocab_set_scope(index == 0 ? lists : (vocab_scope_t){lists.first + index - 1, 1});
    s_view = VIEW_HOME;
    return APP_REDRAW_PAGE;
}

// ---------------------------------------------------------------- quiz

static const char *round_title(void) {
    switch (vocab_round_kind()) {
    case VOCAB_ROUND_WRONG: return "错词复习";
    case VOCAB_ROUND_FORGET: return "防遗忘复习";
    case VOCAB_ROUND_TODAY: return "当天回顾";
    default: return "背单词";
    }
}

static void next_question(void) {
    const int id = vocab_round_word();
    if (id < 0) { s_view = VIEW_DONE; return; }
    s_right = vocab_quiz_options(id, s_options);
    s_picked = -1;
    const vocab_dir_t dir = vocab_dir();
    s_zh_en = dir == VOCAB_DIR_ZH_EN || (dir == VOCAB_DIR_MIXED && (esp_random() & 1u));
    s_view = VIEW_QUIZ;
}

static void progress_label(uint8_t *fb) {
    char line[32];
    const int n = vocab_round_done() + (s_view == VIEW_QUIZ ? 1 : 0);
    snprintf(line, sizeof(line), "%d / %d", n > vocab_round_total() ? vocab_round_total() : n, vocab_round_total());
    ui_text_box(fb, (EpdRect){470, 88, 178, 40}, 24, line, EPD_DRAW_ALIGN_RIGHT, false);
}

static void render_quiz(uint8_t *fb) {
    header(fb, round_title());
    progress_label(fb);
    const int id = vocab_round_word();
    vocab_word_t w;
    if (id < 0 || !vocab_word(id, &w)) return;
    char line[128], pos[48];
    const int level = vocab_word_level(id), wrong = vocab_word_wrong(id);
    snprintf(line, sizeof(line), "%s · %s", s_zh_en ? "看中文选英文" : "看英文选中文",
             vocab_word_in_wrong_book(id) ? "错词" : level == 0 ? "新词" : "复习");
    if (wrong) { const size_t len = strlen(line); snprintf(line + len, sizeof(line) - len, " · 错过 %d 次", wrong); }
    ui_text_box(fb, (EpdRect){X0, 164, W, 60}, 42, line, EPD_DRAW_ALIGN_CENTER, false);

    if (s_zh_en) {
        vocab_short_meaning(w.zh, line, sizeof(line));
        ui_text_box(fb, (EpdRect){X0, 240, W, 110}, 52, line, EPD_DRAW_ALIGN_CENTER, false);
        vocab_pos_name(w.pos, pos, sizeof(pos));
        if (pos[0]) ui_text_box(fb, (EpdRect){X0, 364, W, 40}, 26, pos, EPD_DRAW_ALIGN_CENTER, false);
    } else {
        ui_text_box(fb, (EpdRect){X0, 230, W, 120}, 72, w.en, EPD_DRAW_ALIGN_CENTER, false);
        if (w.ipa[0]) {
            snprintf(line, sizeof(line), "/%s/", w.ipa);
            ui_text_box(fb, (EpdRect){X0, 362, W, 52}, 32, line, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    static const char *const k_letters[4] = {"A", "B", "C", "D"};
    for (int i = 0; i < 4; ++i) {
        const EpdRect r = option_rect(i);
        frame(fb, r, 18);
        vocab_word_t o;
        vocab_word(s_options[i], &o);
        if (s_zh_en) snprintf(line, sizeof(line), "%s", o.en);
        else vocab_short_meaning(o.zh, line, sizeof(line));
        ui_text_box(fb, (EpdRect){r.x + 24, r.y, 40, r.height}, 28, k_letters[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text_box(fb, (EpdRect){r.x + 72, r.y + 14, r.width - 96, r.height - 28}, 34, line, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_draw_button(fb, IDK_BTN, "不知道", false);
}

static app_redraw_t pick(int slot) {
    const int id = vocab_round_word();
    if (id < 0) return APP_REDRAW_NONE;
    s_picked = slot;
    s_shown_id = id;
    vocab_round_answer(slot == s_right ? VOCAB_KNOWN : VOCAB_FORGOT);
    s_view = VIEW_RESULT;
    return APP_REDRAW_PAGE;
}

static app_redraw_t quiz_tap(int x, int y) {
    for (int i = 0; i < 4; ++i)
        if (ui_rect_hit(option_rect(i), x, y)) return pick(i);
    if (ui_rect_hit(IDK_BTN, x, y)) return pick(-1);
    return APP_REDRAW_NONE;
}

// ---------------------------------------------------------------- explanation (result / detail)

static void caption(uint8_t *fb, int y, const char *text) {
    ui_text_fixed(fb, X0 + 4, y, 36, text, EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, y + 52, X0, W, 0xc0);
}
#define CAPTION_H 62

#define BODY_BOTTOM 1080   // 讲解内容不越过底部按钮 / explanation stays above the bottom buttons

// 按剩余高度限制行数。/ Limit lines to the space left above the buttons.
static int fit_lines(int y, int px, int want) {
    const int fit = (BODY_BOTTOM - y) / (px * 4 / 3 + 4);
    return fit < want ? fit : want;
}

// 讲解：词、音标、词性释义、词形变化、例句。字号按电子纸阅读距离放大。返回用到的最低 y。
// Explanation body sized for e-paper reading distance; returns the lowest y used.
static int render_word_body(uint8_t *fb, int id, int y) {
    vocab_word_t w;
    if (!vocab_word(id, &w)) return y;
    char line[160], pos[48];
    // 单词和音标放得下就同一行，给下面的大字腾地方。/ Word and phonetics share a line when they fit.
    snprintf(line, sizeof(line), "/%s/", w.ipa);
    const int ww = ui_text_fixed_width_px(66, w.en), iw = w.ipa[0] ? ui_text_fixed_width_px(34, line) : 0;
    if (ww + iw + 28 <= W) {
        const int x = X0 + (W - ww - iw - (iw ? 28 : 0)) / 2;
        ui_text_fixed_vc(fb, x, y + 46, 66, w.en, EPD_DRAW_ALIGN_LEFT, false);
        if (iw) ui_text_fixed_vc(fb, x + ww + 28, y + 50, 34, line, EPD_DRAW_ALIGN_LEFT, false);
        y += 96;
    } else {
        ui_text_box(fb, (EpdRect){X0, y, W, 92}, 66, w.en, EPD_DRAW_ALIGN_CENTER, false);
        y += 92;
        if (w.ipa[0]) {
            ui_text_box(fb, (EpdRect){X0, y, W, 52}, 34, line, EPD_DRAW_ALIGN_CENTER, false);
            y += 56;
        }
    }
    vocab_pos_name(w.pos, pos, sizeof(pos));
    caption(fb, y, pos[0] ? pos : "释义");
    y = wrap(fb, X0 + 4, y + CAPTION_H, W - 8, 42, w.zh, fit_lines(y + CAPTION_H, 42, 2)) + 6;
    if (w.forms[0]) {
        caption(fb, y, "词形变化");
        y = wrap(fb, X0 + 4, y + CAPTION_H, W - 8, 42, w.forms, fit_lines(y + CAPTION_H, 42, 2)) + 6;   // 两行，给例句留地方 / two lines, leaving room for the example
    }
    caption(fb, y, "例句");
    if (w.ex_en[0]) {
        y = wrap(fb, X0 + 4, y + CAPTION_H, W - 8, 42, w.ex_en, fit_lines(y + CAPTION_H, 42, 3));
        y = wrap(fb, X0 + 4, y + 2, W - 8, 42, w.ex_zh, fit_lines(y + 2, 42, 2));
    } else {
        y = wrap(fb, X0 + 4, y + CAPTION_H, W - 8, 42, "这个词暂时没有例句", 1);
    }
    return y;
}

static void render_result(uint8_t *fb) {
    header(fb, round_title());
    progress_label(fb);
    const bool right = s_picked == s_right;
    char line[160];
    if (right) snprintf(line, sizeof(line), "答对了");
    else if (s_picked < 0) snprintf(line, sizeof(line), "没关系，记一下，已加入错词本");
    else {
        vocab_word_t o;
        vocab_word(s_options[s_picked], &o);
        char m[96];
        if (s_zh_en) snprintf(m, sizeof(m), "%s", o.en);
        else vocab_short_meaning(o.zh, m, sizeof(m));
        snprintf(line, sizeof(line), "答错了，你选的是：%s", m);
    }
    const EpdRect banner = {X0, 160, W, 64};
    if (right) ui_draw_control_frame(fb, banner, 32, 0x70);
    else ui_fill_round_rect(fb, banner, 32, 0x30);
    ui_text_box(fb, ui_inset_rect(banner, 10), 32, line, EPD_DRAW_ALIGN_CENTER, !right);
    render_word_body(fb, s_shown_id, 232);
    ui_draw_button(fb, NEXT_BTN, vocab_round_word() < 0 ? "看本轮结果" : "下一个", true);
}

static app_redraw_t result_next(void) {
    next_question();
    return APP_REDRAW_PAGE;
}

static void render_detail(uint8_t *fb) {
    header(fb, "错词本");
    char line[64];
    snprintf(line, sizeof(line), "累计答错 %d 次", vocab_word_wrong(s_shown_id));
    ui_text_box(fb, (EpdRect){X0, 164, W, 48}, 30, line, EPD_DRAW_ALIGN_CENTER, false);
    render_word_body(fb, s_shown_id, 224);
    ui_draw_button(fb, LEFT_BTN, "移出错词本", false);
    ui_draw_button(fb, RIGHT_BTN, "返回", true);
}

// ---------------------------------------------------------------- wrong book

static int wrong_pages(void) { return s_wrong_count ? (s_wrong_count + LIST_ROWS - 1) / LIST_ROWS : 1; }

static void render_wrong(uint8_t *fb) {
    header(fb, "错词本");
    char line[128];
    if (!s_wrong_count) {
        ui_text_box(fb, (EpdRect){X0, 460, W, 60}, 32, "错词本是空的", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_box(fb, (EpdRect){X0, 530, W, 40}, 22, "答错或选「不知道」的词会自动记到这里", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    snprintf(line, sizeof(line), "开始错词复习（%d 词）",
             s_wrong_count < VOCAB_WRONG_PER_ROUND ? s_wrong_count : VOCAB_WRONG_PER_ROUND);
    ui_draw_button(fb, (EpdRect){X0, 172, W, 84}, line, true);
    for (int row = 0; row < LIST_ROWS; ++row) {
        const int index = s_page * LIST_ROWS + row;
        if (index >= s_wrong_count) break;
        const EpdRect r = {X0, WRONG_TOP + row * WRONG_ROW_H, W, WRONG_ROW_H};
        vocab_word_t w;
        vocab_word(s_wrong_ids[index], &w);
        ui_text_box(fb, (EpdRect){r.x + 10, r.y + 8, 250, r.height - 16}, 30, w.en, EPD_DRAW_ALIGN_LEFT, false);
        vocab_short_meaning(w.zh, line, sizeof(line));
        ui_text_box(fb, (EpdRect){r.x + 270, r.y + 12, 220, r.height - 24}, 22, line, EPD_DRAW_ALIGN_LEFT, false);
        snprintf(line, sizeof(line), "错 %d 次", vocab_word_wrong(s_wrong_ids[index]));
        ui_text_box(fb, (EpdRect){r.x + 496, r.y + 18, 110, r.height - 36}, 20, line, EPD_DRAW_ALIGN_RIGHT, false);
        ui_hairline(fb, r.y + r.height - 1, r.x, r.width, 0xc0);
    }
    if (wrong_pages() > 1) {
        snprintf(line, sizeof(line), "%d/%d", s_page + 1, wrong_pages());
        ui_pager_draw_at(fb, line, s_page, wrong_pages(), false, -1, LIST_PAGER);
    }
    ui_text_box(fb, (EpdRect){X0, LIST_PAGER + 96, W, 32}, 20, "连续答对 3 次自动移出 · 点一行看讲解",
                EPD_DRAW_ALIGN_CENTER, false);
}

static app_redraw_t wrong_tap(int x, int y) {
    if (!s_wrong_count) return APP_REDRAW_NONE;
    if (ui_rect_hit((EpdRect){X0, 172, W, 84}, x, y)) return start_round(VOCAB_ROUND_WRONG);
    const app_redraw_t paged = pager_tap(x, y, wrong_pages());
    if (paged != APP_REDRAW_DONE) return paged;
    if (y < WRONG_TOP || y >= WRONG_TOP + LIST_ROWS * WRONG_ROW_H) return APP_REDRAW_NONE;
    const int index = s_page * LIST_ROWS + (y - WRONG_TOP) / WRONG_ROW_H;
    if (index >= s_wrong_count) return APP_REDRAW_NONE;
    s_shown_id = s_wrong_ids[index];
    s_view = VIEW_DETAIL;
    return APP_REDRAW_PAGE;
}

static app_redraw_t detail_tap(int x, int y) {
    if (ui_rect_hit(LEFT_BTN, x, y)) {
        vocab_wrong_remove(s_shown_id);
        const int page = s_page;
        open_wrong();
        s_page = page < wrong_pages() ? page : wrong_pages() - 1;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(RIGHT_BTN, x, y)) { s_view = VIEW_WRONG; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

// ---------------------------------------------------------------- done

#define DONE_AGAIN ((EpdRect){X0, 620, W, 96})
#define DONE_WRONG ((EpdRect){X0, 736, W, 96})
#define DONE_HOME ((EpdRect){X0, 852, W, 96})

static void render_done(uint8_t *fb) {
    header(fb, round_title());
    int known, vague, forgot;
    vocab_round_counts(&known, &vague, &forgot);
    ui_text_box(fb, (EpdRect){X0, 250, W, 80}, 52, "这一轮完成了", EPD_DRAW_ALIGN_CENTER, false);
    char line[96];
    snprintf(line, sizeof(line), "答对 %d · 答错 %d", known, vague + forgot);
    ui_text_box(fb, (EpdRect){X0, 360, W, 50}, 32, line, EPD_DRAW_ALIGN_CENTER, false);
    vocab_stats_t st;
    vocab_scope_stats(vocab_scope(), &st);
    if (vocab_round_kind() == VOCAB_ROUND_SCOPE)
        snprintf(line, sizeof(line), "本词表已学 %d / %d，掌握 %d", st.learned, st.total, st.mastered);
    else snprintf(line, sizeof(line), "全库今天还有 %d 个到期词", vocab_forget_due());
    ui_text_box(fb, (EpdRect){X0, 430, W, 40}, 24, line, EPD_DRAW_ALIGN_CENTER, false);
    const char *tip = vocab_round_kind() == VOCAB_ROUND_SCOPE ? "20 分钟后做一次「当天回顾」，睡前再做一次"
                    : vocab_round_kind() == VOCAB_ROUND_TODAY ? "回顾越及时，记得越牢"
                    : "答错的词已记入错词本，明天会再出现";
    ui_text_box(fb, (EpdRect){X0, 480, W, 40}, 24, tip, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, DONE_AGAIN, "再来一轮", true);
    const int wrong = vocab_wrong_list(NULL, 0);
    snprintf(line, sizeof(line), "错词复习（%d）", wrong);
    if (wrong) ui_draw_button(fb, DONE_WRONG, line, false);
    ui_draw_button(fb, DONE_HOME, "回到词表", false);
}

static app_redraw_t done_tap(int x, int y) {
    if (ui_rect_hit(DONE_AGAIN, x, y)) return start_round(vocab_round_kind());
    if (ui_rect_hit(DONE_WRONG, x, y) && vocab_wrong_list(NULL, 0)) return start_round(VOCAB_ROUND_WRONG);
    if (ui_rect_hit(DONE_HOME, x, y)) { s_view = VIEW_HOME; return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

// ---------------------------------------------------------------- page

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (!vocab_open()) {
        header(fb, "背单词");
        ui_text_box(fb, (EpdRect){52, 500, 580, 60}, 30, "词库读取失败", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    switch (s_view) {
    case VIEW_BOOK: render_book(fb); break;
    case VIEW_QUIZ: render_quiz(fb); break;
    case VIEW_RESULT: render_result(fb); break;
    case VIEW_DONE: render_done(fb); break;
    case VIEW_WRONG: render_wrong(fb); break;
    case VIEW_DETAIL: render_detail(fb); break;
    default: render_home(fb); break;
    }
}

// 页内返回一层；在首页时离开本页。/ Go up one level inside the page; leave from home.
static app_redraw_t back(app_ctx_t *ctx) {
    vocab_save();
    switch (s_view) {
    case VIEW_HOME: go_back(ctx); return APP_REDRAW_NONE;
    case VIEW_DETAIL: s_view = VIEW_WRONG; return APP_REDRAW_PAGE;
    case VIEW_QUIZ: case VIEW_RESULT: case VIEW_DONE:
        if (vocab_round_kind() == VOCAB_ROUND_WRONG) { open_wrong(); return APP_REDRAW_PAGE; }
        s_view = VIEW_HOME;
        return APP_REDRAW_PAGE;
    default: s_view = VIEW_HOME; return APP_REDRAW_PAGE;
    }
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        const int pages = s_view == VIEW_BOOK ? book_pages() : s_view == VIEW_WRONG ? wrong_pages() : 0;
        if (pages < 2) return APP_REDRAW_NONE;
        const int target = s_page + (ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        if (target < 0 || target >= pages) return APP_REDRAW_NONE;
        s_page = target;
        return APP_REDRAW_PAGE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const int x = ev->x0, y = ev->y0;
    if (y < 160 && x < 160) return back(ctx);
    if (!vocab_open()) return APP_REDRAW_NONE;
    switch (s_view) {
    case VIEW_BOOK: return book_tap(x, y);
    case VIEW_QUIZ: return quiz_tap(x, y);
    case VIEW_RESULT: return ui_rect_hit(NEXT_BTN, x, y) ? result_next() : APP_REDRAW_NONE;
    case VIEW_DONE: return done_tap(x, y);
    case VIEW_WRONG: return wrong_tap(x, y);
    case VIEW_DETAIL: return detail_tap(x, y);
    default: return home_tap(x, y);
    }
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (key == UI_KEY_1 || key == UI_KEY_3) {
        // 讲解页：音量键都是「下一个」。列表页：翻页。/ Explanation: either volume key goes next. Lists: page.
        if (s_view == VIEW_RESULT) return result_next();
        const int pages = s_view == VIEW_BOOK ? book_pages() : s_view == VIEW_WRONG ? wrong_pages() : 0;
        if (pages > 1) {
            const int target = s_page + (key == UI_KEY_3 ? 1 : -1);
            if (target >= 0 && target < pages) { s_page = target; return APP_REDRAW_PAGE; }
        }
        return APP_REDRAW_NONE;
    }
    return back(ctx);
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    (void)ctx;
    return key == UI_KEY_2 ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_view = VIEW_HOME;
    s_stage = 0;
    // 打开时停在当前词表所在的阶段。/ Open on the stage holding the current list.
    if (vocab_open()) {
        const int book = vocab_list_book(vocab_scope().first);
        for (int i = 0; i < vocab_stage_count(); ++i) {
            const vocab_scope_t b = vocab_stage_books(i);
            if (book >= b.first && book < b.first + b.count) s_stage = i;
        }
    }
}

static void vocab_exit(app_ctx_t *ctx) { (void)ctx; vocab_save(); }
static void on_before_lock(app_ctx_t *ctx) { (void)ctx; vocab_save(); }

/// 「应用」页卡片上的两行摘要。/ Two summary lines for the Apps card.
void app_vocab_summary(char *line1, int size1, char *line2, int size2) {
    if (!vocab_open()) {
        snprintf(line1, size1, "内置词库");
        snprintf(line2, size2, "按记忆曲线复习");
        return;
    }
    vocab_scope_name(vocab_scope(), line1, size1);
    vocab_stats_t st;
    vocab_scope_stats(vocab_scope(), &st);
    const int due = vocab_forget_due(), wrong = vocab_wrong_list(NULL, 0);
    if (st.learned == 0 && !due) snprintf(line2, size2, "%d 词，点开开始", st.total);
    else if (wrong) snprintf(line2, size2, "待复习 %d · 错词 %d", due, wrong);
    else snprintf(line2, size2, "待复习 %d · 已学 %d", due, st.learned);
}

const app_desc_t app_vocab = {
    .title = "背单词",
    .detail = "人教版小学、中考词汇与常用 2000 词",
    .binary_ui = true,
    .owns_keys = true,
    .on_enter = on_enter,
    .on_exit = vocab_exit,
    .on_before_lock = on_before_lock,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
};
