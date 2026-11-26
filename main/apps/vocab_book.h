/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：背单词的词库与进度。词库是固件内置的 vocab.bin（tools/gen_vocab.py 生成）：阶段 → 册 → 单元/组；
 *       单元在文件里按顺序排好，所以「一个单元 / 一册 / 一个年级 / 整个阶段」都是一段连续的单元区间。
 *       每个词带音标、释义、词性、词形变化和一句中英例句。
 *       进度按词记录记忆等级、下次复习日（间隔 1/2/4/7/15/30 天）和错词情况，存在 /flash/vocab.dat。
 *       错词本：答错过且之后没有连续答对 3 次的词。防遗忘：全部词库里今天到期该复习的词。
 * English: Vocabulary book and progress. The book is the built-in vocab.bin (from tools/gen_vocab.py):
 *       stage → book → unit/group, stored in order, so a unit, a book, a grade or a whole stage is one
 *       contiguous range of lists. Each word carries phonetics, meaning, part of speech, forms and an
 *       English–Chinese example. Progress keeps a level, a next-review day (1/2/4/7/15/30 days) and
 *       mistakes per word in /flash/vocab.dat. The wrong-word book holds words missed and not yet
 *       answered right three times in a row; anti-forgetting reviews every word due today.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOCAB_LEVEL_MAX 6
#define VOCAB_MASTERED 5
#define VOCAB_NEW_PER_ROUND 20
#define VOCAB_REVIEW_PER_ROUND 100
#define VOCAB_WRONG_PER_ROUND 30
#define VOCAB_WRONG_CLEAR 3   // 连续答对几次移出错词本 / right answers in a row that clear a wrong word

typedef struct { const char *en, *ipa, *zh, *pos, *forms, *ex_en, *ex_zh; } vocab_word_t;
typedef struct { int first, count; } vocab_scope_t;   // 单元区间 / range of lists
typedef struct { int total, learned, mastered, due, fresh; } vocab_stats_t;
typedef enum { VOCAB_FORGOT = 0, VOCAB_VAGUE, VOCAB_KNOWN } vocab_grade_t;
typedef enum { VOCAB_ROUND_SCOPE = 0, VOCAB_ROUND_WRONG, VOCAB_ROUND_FORGET, VOCAB_ROUND_TODAY } vocab_round_kind_t;
/// 当天回顾的进度：今天新学或答错的词数、已做到第几次（0/1/2）、第一次还要等几分钟、是否已到晚上。
/// Same-day review: words learned or missed today, reviews done (0/1/2), minutes to wait for the first, evening or not.
typedef struct { int count, done, wait_min; bool evening; } vocab_today_t;
typedef enum { VOCAB_DIR_MIXED = 0, VOCAB_DIR_EN_ZH, VOCAB_DIR_ZH_EN } vocab_dir_t;

/// 解析内置词库并读入进度；重复调用无副作用。/ Parse the built-in book and load progress; idempotent.
bool vocab_open(void);

int vocab_stage_count(void);
const char *vocab_stage_name(int stage);
vocab_scope_t vocab_stage_books(int stage);   // 册区间 / range of books
int vocab_book_count(void);
const char *vocab_book_name(int book);
vocab_scope_t vocab_book_lists(int book);
int vocab_list_count(void);
const char *vocab_list_name(int list);
int vocab_list_size(int list);
int vocab_list_book(int list);
int vocab_list_word(int list, int k);
int vocab_word_count(void);
bool vocab_word(int id, vocab_word_t *out);
int vocab_word_level(int id);
int vocab_word_wrong(int id);      // 累计答错次数 / total mistakes
bool vocab_word_in_wrong_book(int id);

/// 当前学习范围及其名称。/ Current study scope and its display name.
vocab_scope_t vocab_scope(void);
void vocab_set_scope(vocab_scope_t scope);
void vocab_scope_name(vocab_scope_t scope, char *out, int size);
void vocab_scope_stats(vocab_scope_t scope, vocab_stats_t *out);

/// 答题方向（混合 / 看英文选中文 / 看中文选英文），随进度保存。/ Quiz direction, saved with progress.
vocab_dir_t vocab_dir(void);
void vocab_set_dir(vocab_dir_t dir);

/// 每日新词量（5/8/10/15，默认 8）与今天已学新词数、连续打卡天数。
/// Daily new-word quota (5/8/10/15, default 8), new words learned today and the study streak in days.
int vocab_daily_new(void);
void vocab_cycle_daily_new(void);
int vocab_new_today(void);
int vocab_streak(void);
/// 当天回顾状态。/ Same-day review status.
void vocab_today_status(vocab_today_t *out);

/// 错词本：按答错次数从多到少。返回总数，ids 可为 NULL。/ Wrong book, most-missed first; returns the count.
int vocab_wrong_list(uint16_t *ids, int max);
void vocab_wrong_remove(int id);
/// 全部词库今天到期的词数（防遗忘）。/ Words due today across the whole library (anti-forgetting).
int vocab_forget_due(void);

/// 组一轮。本范围：先到期复习，再新词；错词：错词本；防遗忘：全库到期词，拖得最久的先。返回张数。
/// 当天回顾：今天新学和答错的词，学完 20 分钟后一次、晚上 7 点后一次；答对不升级。
/// Build a round: scope = due then new; wrong = the wrong book; forget = all due words, most overdue first;
/// today = words learned or missed today, once 20 minutes later and once after 7 pm, without promoting levels.
int vocab_round_start(vocab_round_kind_t kind);
vocab_round_kind_t vocab_round_kind(void);
int vocab_round_word(void);      // 当前词，-1 为本轮结束 / current word, -1 when done
int vocab_round_done(void);
int vocab_round_total(void);
void vocab_round_answer(vocab_grade_t grade);
void vocab_round_counts(int *known, int *vague, int *forgot);
/// 为 id 出一道四选一：out[0..3] 是选项词，返回正确项下标。/ Build a 4-choice question; returns the right slot.
int vocab_quiz_options(int id, int out[4]);
/// 释义的简短版（去掉词性前缀，取第一组义项）。/ Short meaning: no POS prefix, first sense group only.
void vocab_short_meaning(const char *zh, char *out, int size);
/// 词性代码转中文（"v n" → "动词 / 名词"）。/ POS codes to Chinese names.
void vocab_pos_name(const char *pos, char *out, int size);
/// 有未保存的改动时写回 /flash。/ Write progress back to /flash when it changed.
void vocab_save(void);

#ifdef __cplusplus
}
#endif
