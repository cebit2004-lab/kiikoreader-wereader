/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：背单词词库与进度，见 vocab_book.h。/ English: vocab book and progress, see vocab_book.h.
 */
#include "vocab_book.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "book_store.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "vocab";
extern const uint8_t vocab_bin_start[] asm("_binary_vocab_bin_start");
extern const uint8_t vocab_bin_end[] asm("_binary_vocab_bin_end");

#define PROGRESS_PATH "/flash/vocab.dat"
#define PROGRESS_TMP "/flash/vocab.tmp"
#define ROUND_MAX (VOCAB_NEW_PER_ROUND + VOCAB_REVIEW_PER_ROUND + 40)
#define RETRY_GAP 4   // 答错的词隔几张再出 / cards before a missed word comes back

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t stages, books, lists, words;
    uint32_t ids;
    uint32_t off_stage, off_book, off_list, off_ids, off_word, off_str;
} vocab_head_t;
typedef struct __attribute__((packed)) { uint32_t name; uint16_t first, count; } vocab_group_t;
typedef struct __attribute__((packed)) { uint32_t name, first; uint16_t count, pad; } vocab_list_t;

// 每词进度。/ Per-word progress.
// touch：最近一次新学或答错的日子，当天回顾按它挑词。/ touch: the last day the word was new or missed.
typedef struct __attribute__((packed)) { uint8_t level, seen, wrong, streak; uint16_t due, touch; } vocab_progress_t;
typedef struct __attribute__((packed)) { uint8_t level, seen, wrong, streak; uint16_t due; } vocab_progress_v2_t;
typedef struct __attribute__((packed)) { uint8_t level, seen; uint16_t due; } vocab_progress_v1_t;
// 头部之后的每日状态。/ Daily state after the header.
typedef struct __attribute__((packed)) {
    uint8_t dir, daily_new, review_done, pad;
    uint16_t stat_day, new_today, streak, study_day;
    uint32_t last_learn;   // 最近一次学新词或答错的时间（Unix 秒）/ last new or missed answer, Unix seconds
} daily_t;
static const uint8_t k_daily_options[] = {5, 8, 10, 15};
#define DAILY_DEFAULT 8
#define REVIEW_WAIT_S (20 * 60)
#define EVENING_HOUR 19
typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t book_sig;
    uint16_t words, scope_first, scope_count, last_day;
} progress_head_t;

static const vocab_head_t *s_head;
static const vocab_group_t *s_stage, *s_book;
static const vocab_list_t *s_list;
static const uint16_t *s_ids;
static const uint32_t *s_word;
static const char *s_str;
static vocab_progress_t *s_prog;
static vocab_scope_t s_scope;
static uint16_t s_last_day;
static vocab_dir_t s_dir;
static daily_t s_daily = {.daily_new = DAILY_DEFAULT};
static bool s_dirty;

static uint16_t s_round[ROUND_MAX];
static uint8_t s_retry[ROUND_MAX];
static int s_round_len, s_round_pos, s_round_total, s_known, s_vague, s_forgot;
static vocab_round_kind_t s_round_kind;

static const char *str(uint32_t off) { return s_str + off; }

// 词库签名：全部英文词条的 FNV 哈希。词条不变时（只补例句、词形）旧进度照用。
// Book signature: FNV over all headwords, so progress survives regenerations that only add examples or forms.
static uint32_t book_sig(void) {
    uint32_t h = 2166136261u ^ s_head->words;
    for (int i = 0; i < s_head->words; ++i)
        for (const char *p = str(s_word[i]); *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

// 北京时间的天数；时间未同步时沿用上次记下的日子。/ Day number in Beijing time; reuse the last day while unsynced.
static uint16_t today(void) {
    const time_t now = time(NULL);
    if (now >= 1704067200) {
        const uint16_t day = (uint16_t)((now + 8 * 3600) / 86400);
        if (day != s_last_day) { s_last_day = day; s_dirty = true; }
    }
    if (s_daily.stat_day != s_last_day) {
        // 新的一天：清零今日计数。/ A new day: reset today's counters.
        s_daily.stat_day = s_last_day;
        s_daily.new_today = 0;
        s_daily.review_done = 0;
        s_dirty = true;
    }
    return s_last_day;
}

static bool is_due(int id) { return s_prog[id].level > 0 && (int16_t)(s_prog[id].due - s_last_day) <= 0; }

static void load_progress(void) {
    s_scope = (vocab_scope_t){0, s_book[0].count};
    if (book_store_flash_mount() != ESP_OK) return;
    FILE *f = fopen(PROGRESS_PATH, "rb");
    if (!f) return;
    progress_head_t h;
    bool ok = false;
    if (fread(&h, sizeof(h), 1, f) == 1 && h.words == s_head->words && h.book_sig == book_sig()) {
        if (!memcmp(h.magic, "VPR3", 4)) {
            ok = fread(&s_daily, sizeof(s_daily), 1, f) == 1 &&
                 fread(s_prog, sizeof(*s_prog), s_head->words, f) == s_head->words;
            if (ok && s_daily.dir <= VOCAB_DIR_ZH_EN) s_dir = (vocab_dir_t)s_daily.dir;
        } else if (!memcmp(h.magic, "VPR2", 4)) {
            uint8_t extra[4];
            vocab_progress_v2_t old;
            ok = fread(extra, sizeof(extra), 1, f) == 1;
            if (ok && extra[0] <= VOCAB_DIR_ZH_EN) s_dir = (vocab_dir_t)extra[0];
            for (int i = 0; i < s_head->words && ok; ++i) {
                ok = fread(&old, sizeof(old), 1, f) == 1;
                s_prog[i] = (vocab_progress_t){old.level, old.seen, old.wrong, old.streak, old.due, 0};
            }
            s_dirty = true;
        } else if (!memcmp(h.magic, "VPR1", 4)) {
            // 旧版只有等级和复习日。/ The first format had level and due day only.
            vocab_progress_v1_t old;
            ok = true;
            for (int i = 0; i < s_head->words && ok; ++i) {
                ok = fread(&old, sizeof(old), 1, f) == 1;
                s_prog[i] = (vocab_progress_t){.level = old.level, .seen = old.seen, .due = old.due};
            }
            s_dirty = true;
        }
    }
    if (ok) {
        bool known = false;
        for (size_t i = 0; i < sizeof(k_daily_options); ++i) known |= s_daily.daily_new == k_daily_options[i];
        if (!known) s_daily.daily_new = DAILY_DEFAULT;
        if (h.scope_count > 0 && h.scope_first + h.scope_count <= s_head->lists)
            s_scope = (vocab_scope_t){h.scope_first, h.scope_count};
        s_last_day = h.last_day;
    } else {
        ESP_LOGW(TAG, "progress does not match this book, starting fresh");
        memset(s_prog, 0, sizeof(*s_prog) * s_head->words);
    }
    fclose(f);
}

void vocab_save(void) {
    if (!s_prog || !s_dirty) return;
    if (book_store_flash_mount() != ESP_OK) return;
    FILE *f = fopen(PROGRESS_TMP, "wb");
    if (!f) { ESP_LOGW(TAG, "save: open failed"); return; }
    progress_head_t h = {.magic = {'V', 'P', 'R', '3'}, .book_sig = book_sig(), .words = s_head->words,
                         .scope_first = (uint16_t)s_scope.first, .scope_count = (uint16_t)s_scope.count,
                         .last_day = s_last_day};
    s_daily.dir = (uint8_t)s_dir;
    const bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&s_daily, sizeof(s_daily), 1, f) == 1 &&
                    fwrite(s_prog, sizeof(*s_prog), s_head->words, f) == s_head->words;
    if (fclose(f) != 0 || !ok) { remove(PROGRESS_TMP); ESP_LOGW(TAG, "save: write failed"); return; }
    remove(PROGRESS_PATH);
    if (rename(PROGRESS_TMP, PROGRESS_PATH) != 0) { ESP_LOGW(TAG, "save: rename failed"); return; }
    s_dirty = false;
}

bool vocab_open(void) {
    if (s_head) return s_prog != NULL;
    const vocab_head_t *h = (const vocab_head_t *)vocab_bin_start;
    const size_t size = (size_t)(vocab_bin_end - vocab_bin_start);
    if (size < sizeof(*h) || memcmp(h->magic, "VOC2", 4) || h->off_str >= size || h->stages == 0 || h->books == 0) {
        ESP_LOGE(TAG, "built-in book is damaged");
        return false;
    }
    s_head = h;
    s_stage = (const vocab_group_t *)(vocab_bin_start + h->off_stage);
    s_book = (const vocab_group_t *)(vocab_bin_start + h->off_book);
    s_list = (const vocab_list_t *)(vocab_bin_start + h->off_list);
    s_ids = (const uint16_t *)(vocab_bin_start + h->off_ids);
    s_word = (const uint32_t *)(vocab_bin_start + h->off_word);
    s_str = (const char *)(vocab_bin_start + h->off_str);
    s_prog = calloc(h->words, sizeof(*s_prog));
    if (!s_prog) { ESP_LOGE(TAG, "no memory for progress"); return false; }
    load_progress();
    ESP_LOGI(TAG, "book: %u words in %u lists; scope %d+%d", h->words, h->lists, s_scope.first, s_scope.count);
    return true;
}

int vocab_stage_count(void) { return s_head ? s_head->stages : 0; }
const char *vocab_stage_name(int i) { return str(s_stage[i].name); }
vocab_scope_t vocab_stage_books(int i) { return (vocab_scope_t){s_stage[i].first, s_stage[i].count}; }
int vocab_book_count(void) { return s_head ? s_head->books : 0; }
const char *vocab_book_name(int i) { return str(s_book[i].name); }
vocab_scope_t vocab_book_lists(int i) { return (vocab_scope_t){s_book[i].first, s_book[i].count}; }
int vocab_list_count(void) { return s_head ? s_head->lists : 0; }
const char *vocab_list_name(int i) { return str(s_list[i].name); }
int vocab_list_size(int i) { return s_list[i].count; }
int vocab_list_word(int list, int k) { return k >= 0 && k < s_list[list].count ? s_ids[s_list[list].first + k] : -1; }
int vocab_word_count(void) { return s_head ? s_head->words : 0; }
static bool valid_id(int id) { return s_prog && id >= 0 && id < s_head->words; }
int vocab_word_level(int id) { return valid_id(id) ? s_prog[id].level : 0; }
int vocab_word_wrong(int id) { return valid_id(id) ? s_prog[id].wrong : 0; }
bool vocab_word_in_wrong_book(int id) {
    return valid_id(id) && s_prog[id].wrong > 0 && s_prog[id].streak < VOCAB_WRONG_CLEAR;
}

int vocab_list_book(int list) {
    for (int b = 0; b < s_head->books; ++b)
        if (list >= s_book[b].first && list < s_book[b].first + s_book[b].count) return b;
    return 0;
}

bool vocab_word(int id, vocab_word_t *out) {
    if (!s_head || id < 0 || id >= s_head->words) return false;
    const char **field[7] = {&out->en, &out->ipa, &out->zh, &out->pos, &out->forms, &out->ex_en, &out->ex_zh};
    const char *p = str(s_word[id]);
    for (int i = 0; i < 7; ++i) { *field[i] = p; p += strlen(p) + 1; }
    return true;
}

vocab_scope_t vocab_scope(void) { return s_scope; }
void vocab_set_scope(vocab_scope_t scope) {
    if (scope.count <= 0 || scope.first < 0 || scope.first + scope.count > s_head->lists) return;
    if (scope.first == s_scope.first && scope.count == s_scope.count) return;
    s_scope = scope;
    s_dirty = true;
    vocab_save();
}

vocab_dir_t vocab_dir(void) { return s_dir; }
void vocab_set_dir(vocab_dir_t dir) {
    if (dir == s_dir || dir > VOCAB_DIR_ZH_EN) return;
    s_dir = dir;
    s_dirty = true;
    vocab_save();
}

// 名称：单元写「五年级上册 · Unit 3」，整册写册名，跨册按阶段或年级。
// Name: "book · unit" for one list, the book name for a whole book, otherwise the grade or stage.
void vocab_scope_name(vocab_scope_t sc, char *out, int size) {
    const int b0 = vocab_list_book(sc.first), b1 = vocab_list_book(sc.first + sc.count - 1);
    const vocab_scope_t book = vocab_book_lists(b0);
    if (b0 == b1 && sc.count == 1 && book.count > 1) {
        snprintf(out, size, "%s · %s", vocab_book_name(b0), vocab_list_name(sc.first));
        return;
    }
    if (b0 == b1 && sc.first == book.first && sc.count == book.count) {
        snprintf(out, size, "%s", vocab_book_name(b0));
        return;
    }
    for (int s = 0; s < s_head->stages; ++s) {
        const vocab_scope_t books = vocab_stage_books(s);
        if (b0 == books.first && b1 == books.first + books.count - 1) {
            snprintf(out, size, "%s全部", vocab_stage_name(s));
            return;
        }
    }
    // 小学同一年级的上下两册。/ Both terms of one primary grade.
    const char *n0 = vocab_book_name(b0);
    if (b1 == b0 + 1 && !strncmp(n0, vocab_book_name(b1), strlen("三年级"))) {
        snprintf(out, size, "%.*s全年", (int)strlen("三年级"), n0);
        return;
    }
    snprintf(out, size, "%s 等 %d 组", vocab_list_name(sc.first), sc.count);
}

// 遍历范围内的词，去重。/ Visit each word of the scope once.
static void for_scope(vocab_scope_t sc, void (*visit)(int id, void *arg), void *arg) {
    uint8_t *seen = calloc((s_head->words + 7) / 8, 1);
    if (!seen) return;
    for (int l = sc.first; l < sc.first + sc.count; ++l)
        for (int k = 0; k < s_list[l].count; ++k) {
            const int id = s_ids[s_list[l].first + k];
            if (seen[id >> 3] & (1u << (id & 7))) continue;
            seen[id >> 3] |= (uint8_t)(1u << (id & 7));
            visit(id, arg);
        }
    free(seen);
}

static void count_word(int id, void *arg) {
    vocab_stats_t *st = arg;
    ++st->total;
    if (s_prog[id].level == 0) { ++st->fresh; return; }
    ++st->learned;
    if (s_prog[id].level >= VOCAB_MASTERED) ++st->mastered;
    if (is_due(id)) ++st->due;
}

void vocab_scope_stats(vocab_scope_t sc, vocab_stats_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_prog) return;
    today();
    for_scope(sc, count_word, out);
}

int vocab_daily_new(void) { return s_daily.daily_new; }
void vocab_cycle_daily_new(void) {
    size_t i = 0;
    while (i < sizeof(k_daily_options) && k_daily_options[i] != s_daily.daily_new) ++i;
    s_daily.daily_new = k_daily_options[(i + 1) % sizeof(k_daily_options)];
    s_dirty = true;
    vocab_save();
}
int vocab_new_today(void) { today(); return s_daily.new_today; }
int vocab_streak(void) {
    // 昨天或今天学过才算连续。/ The streak counts only if studied today or yesterday.
    const uint16_t day = today();
    return s_daily.study_day == day || (uint16_t)(s_daily.study_day + 1) == day ? s_daily.streak : 0;
}

static bool clock_valid(void) { return time(NULL) >= 1704067200; }

void vocab_today_status(vocab_today_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_prog) return;
    const uint16_t day = today();
    for (int id = 0; id < s_head->words; ++id) out->count += s_prog[id].level > 0 && s_prog[id].touch == day;
    out->done = s_daily.review_done;
    if (!clock_valid()) { out->evening = true; return; }   // 没对时就不卡时间 / no clock, no waiting
    const time_t now = time(NULL);
    const long left = (long)s_daily.last_learn + REVIEW_WAIT_S - (long)now;
    out->wait_min = left > 0 ? (int)((left + 59) / 60) : 0;
    out->evening = ((now + 8 * 3600) % 86400) / 3600 >= EVENING_HOUR;
}

int vocab_wrong_list(uint16_t *ids, int max) {
    if (!s_prog) return 0;
    int n = 0;
    for (int id = 0; id < s_head->words; ++id) {
        if (!vocab_word_in_wrong_book(id)) continue;
        if (ids && n < max) {
            // 插入排序：错得多的在前。/ Insertion sort, most missed first.
            int at = n;
            while (at > 0 && s_prog[ids[at - 1]].wrong < s_prog[id].wrong) { ids[at] = ids[at - 1]; --at; }
            ids[at] = (uint16_t)id;
        }
        ++n;
    }
    return n;
}

void vocab_wrong_remove(int id) {
    if (!vocab_word_in_wrong_book(id)) return;
    s_prog[id].streak = VOCAB_WRONG_CLEAR;
    s_dirty = true;
    vocab_save();
}

int vocab_forget_due(void) {
    if (!s_prog) return 0;
    today();
    int n = 0;
    for (int id = 0; id < s_head->words; ++id) n += is_due(id);
    return n;
}

typedef struct { int reviews, fresh; uint16_t review_ids[VOCAB_REVIEW_PER_ROUND]; uint16_t fresh_ids[VOCAB_NEW_PER_ROUND]; } pick_t;

// 复习名额满了就换掉最晚到期的，留下拖得最久的。/ When reviews are full, keep the longest-overdue ones.
static void add_review(pick_t *p, int id) {
    if (p->reviews < VOCAB_REVIEW_PER_ROUND) { p->review_ids[p->reviews++] = (uint16_t)id; return; }
    int late = 0;
    for (int i = 1; i < p->reviews; ++i)
        if ((int16_t)(s_prog[p->review_ids[i]].due - s_prog[p->review_ids[late]].due) > 0) late = i;
    if ((int16_t)(s_prog[id].due - s_prog[p->review_ids[late]].due) < 0) p->review_ids[late] = (uint16_t)id;
}

static void pick_word(int id, void *arg) {
    pick_t *p = arg;
    if (s_prog[id].level == 0) {
        if (p->fresh < VOCAB_NEW_PER_ROUND) p->fresh_ids[p->fresh++] = (uint16_t)id;
        return;
    }
    if (is_due(id)) add_review(p, id);
}

static void shuffle(uint16_t *a, int n) {
    for (int i = n - 1; i > 0; --i) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const uint16_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

int vocab_round_start(vocab_round_kind_t kind) {
    s_round_len = s_round_pos = s_known = s_vague = s_forgot = 0;
    s_round_kind = kind;
    if (!s_prog) return 0;
    today();
    if (kind == VOCAB_ROUND_WRONG) {
        s_round_len = vocab_wrong_list(s_round, VOCAB_WRONG_PER_ROUND);
        if (s_round_len > VOCAB_WRONG_PER_ROUND) s_round_len = VOCAB_WRONG_PER_ROUND;
        shuffle(s_round, s_round_len);
    } else {
        pick_t *p = calloc(1, sizeof(*p));
        if (!p) return 0;
        if (kind == VOCAB_ROUND_FORGET) {
            for (int id = 0; id < s_head->words; ++id) if (is_due(id)) add_review(p, id);
        } else if (kind == VOCAB_ROUND_TODAY) {
            for (int id = 0; id < s_head->words; ++id)
                if (s_prog[id].level > 0 && s_prog[id].touch == s_last_day && p->reviews < VOCAB_REVIEW_PER_ROUND)
                    p->review_ids[p->reviews++] = (uint16_t)id;
        } else {
            for_scope(s_scope, pick_word, p);
            // 每日新词量用完就只复习。/ Once today's quota is used, review only.
            const int left = s_daily.daily_new > s_daily.new_today ? s_daily.daily_new - s_daily.new_today : 0;
            if (p->fresh > left) p->fresh = left;
        }
        shuffle(p->review_ids, p->reviews);
        // 复习和新词穿插：每两张复习带一个新词。/ Interleave: one new word after every two reviews.
        int r = 0, n = 0;
        while (r < p->reviews || n < p->fresh) {
            for (int k = 0; k < 2 && r < p->reviews; ++k) s_round[s_round_len++] = p->review_ids[r++];
            if (n < p->fresh) s_round[s_round_len++] = p->fresh_ids[n++];
        }
        free(p);
    }
    memset(s_retry, 0, sizeof(s_retry));
    s_round_total = s_round_len;
    ESP_LOGI(TAG, "round %d: %d cards", (int)kind, s_round_len);
    return s_round_len;
}

vocab_round_kind_t vocab_round_kind(void) { return s_round_kind; }
int vocab_round_word(void) { return s_round_pos < s_round_len ? s_round[s_round_pos] : -1; }
int vocab_round_done(void) { return s_known + s_vague + s_forgot; }
int vocab_round_total(void) { return s_round_total; }
void vocab_round_counts(int *known, int *vague, int *forgot) { *known = s_known; *vague = s_vague; *forgot = s_forgot; }

static const uint8_t k_gap_days[VOCAB_LEVEL_MAX + 1] = {0, 1, 2, 4, 7, 15, 30};

void vocab_round_answer(vocab_grade_t grade) {
    const int id = vocab_round_word();
    if (id < 0) return;
    vocab_progress_t *p = &s_prog[id];
    const bool retry = s_retry[s_round_pos];
    const uint16_t day = today();
    // 打卡：今天第一次答题时更新连续天数。/ Streak: update on the first answer of the day.
    if (s_daily.study_day != day) {
        s_daily.streak = (uint16_t)(s_daily.study_day + 1) == day ? s_daily.streak + 1 : 1;
        s_daily.study_day = day;
    }
    if (p->level == 0 && !retry) {
        ++s_daily.new_today;
        p->touch = day;
        s_daily.last_learn = (uint32_t)time(NULL);
    }
    if (s_round_kind == VOCAB_ROUND_TODAY && grade == VOCAB_KNOWN) {
        // 当天回顾答对不升级，只算巩固。/ A right same-day review consolidates without promoting.
        if (!retry) ++s_known;
    } else if (grade == VOCAB_KNOWN) {
        // 新词一眼就认识直接跳两级；本轮重做的词只回到 1 级。/ New and right at once skips a level; retries stay at 1.
        p->level = p->level == 0 ? 2 : p->level < VOCAB_LEVEL_MAX ? p->level + 1 : VOCAB_LEVEL_MAX;
        if (retry) p->level = 1;
        p->due = day + k_gap_days[p->level];
        if (p->wrong && p->streak < VOCAB_WRONG_CLEAR && !retry) ++p->streak;
        if (!retry) ++s_known;
    } else if (grade == VOCAB_VAGUE) {
        p->level = p->level > 1 ? p->level - 1 : 1;
        p->due = day + 1;
        if (!retry) ++s_vague;
    } else {
        p->level = 1;
        p->due = day;
        p->touch = day;
        if (s_round_kind != VOCAB_ROUND_TODAY) s_daily.last_learn = (uint32_t)time(NULL);
        if (p->wrong < 255) ++p->wrong;
        p->streak = 0;
        if (!retry) ++s_forgot;
        // 本轮稍后再出一次。/ Show it again later in this round.
        if (s_round_len < ROUND_MAX) {
            int at = s_round_pos + 1 + RETRY_GAP;
            if (at > s_round_len) at = s_round_len;
            memmove(&s_round[at + 1], &s_round[at], (size_t)(s_round_len - at) * sizeof(s_round[0]));
            memmove(&s_retry[at + 1], &s_retry[at], (size_t)(s_round_len - at));
            s_round[at] = (uint16_t)id;
            s_retry[at] = 1;
            ++s_round_len;
        }
    }
    if (p->seen < 255) ++p->seen;
    s_dirty = true;
    ++s_round_pos;
    if (vocab_round_word() < 0 && s_round_kind == VOCAB_ROUND_TODAY && s_daily.review_done < 2) ++s_daily.review_done;
    if (vocab_round_done() % 10 == 0 || vocab_round_word() < 0) vocab_save();
}

// ---------------------------------------------------------------- quiz helpers

void vocab_short_meaning(const char *zh, char *out, int size) {
    // 去掉开头的「n. 」「vt. 」等词性前缀。/ Drop a leading POS prefix such as "n. " or "vt. ".
    const char *p = zh;
    while (*p >= 'a' && *p <= 'z') ++p;
    if (p != zh && *p == '.') { ++p; while (*p == ' ') ++p; } else p = zh;
    snprintf(out, size, "%s", p);
    char *cut = strstr(out, "；");
    if (cut && cut != out) *cut = 0;
}

void vocab_pos_name(const char *pos, char *out, int size) {
    static const char *const codes[][2] = {{"n", "名词"}, {"v", "动词"}, {"a", "形容词"}, {"adv", "副词"},
                                           {"prep", "介词"}, {"pron", "代词"}, {"conj", "连词"}, {"num", "数词"},
                                           {"int", "感叹词"}, {"art", "冠词"}, {"phr", "短语"}};
    out[0] = 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", pos);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " "))
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
            if (!strcmp(tok, codes[i][0])) {
                const int len = (int)strlen(out);
                snprintf(out + len, size - len, "%s%s", len ? " / " : "", codes[i][1]);
            }
}

static bool same_first_pos(const char *a, const char *b) {
    const size_t la = strcspn(a, " "), lb = strcspn(b, " ");
    return la == lb && !strncmp(a, b, la);
}

int vocab_quiz_options(int id, int out[4]) {
    vocab_word_t w, c;
    if (!vocab_word(id, &w)) return 0;
    char mine[96], other[96];
    vocab_short_meaning(w.zh, mine, sizeof(mine));
    int n = 1;
    out[0] = id;
    // 干扰项先在附近（同一册附近的词）找同词性的，找不到再放宽。
    // Distractors: nearby words (same book region) with the same POS first, then anything nearby.
    for (int pass = 0; pass < 3 && n < 4; ++pass) {
        const int span = pass == 0 ? 150 : pass == 1 ? 400 : s_head->words;
        for (int tries = 0; tries < 60 && n < 4; ++tries) {
            int cand = id - span + (int)(esp_random() % (uint32_t)(2 * span + 1));
            if (cand < 0 || cand >= s_head->words || !vocab_word(cand, &c)) continue;
            if (pass < 2 && !same_first_pos(w.pos, c.pos)) continue;
            if (!strcasecmp(c.en, w.en)) continue;
            vocab_short_meaning(c.zh, other, sizeof(other));
            if (!strcmp(other, mine)) continue;
            bool dup = false;
            for (int k = 0; k < n; ++k) {
                vocab_word_t o;
                vocab_word(out[k], &o);
                char om[96];
                vocab_short_meaning(o.zh, om, sizeof(om));
                if (out[k] == cand || !strcasecmp(o.en, c.en) || !strcmp(om, other)) dup = true;
            }
            if (!dup) out[n++] = cand;
        }
    }
    for (; n < 4; ++n) out[n] = (id + n * 97) % s_head->words;   // 极端情况兜底 / last resort
    const int right = (int)(esp_random() % 4u);
    const int t = out[right]; out[right] = out[0]; out[0] = t;
    return right;
}
