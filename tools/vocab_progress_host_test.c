/* SPDX-License-Identifier: Apache-2.0
 * 背单词进度文件主机测试：VPR5 稀疏格式的存取、只写学过的词、VPR4/VPR3 迁移、词库变化后按词头哈希对回、
 * tmp/bak 恢复、坏文件挪开不被覆盖、日子只往前走，以及内置词库检查。
 * Host test for the vocab progress file: VPR5 sparse round trip, touched-only records, VPR4/VPR3
 * migration, remap by headword hash after the book changes, tmp/bak recovery, damaged files set aside
 * and never overwritten, the day only moving forward, and the built-in book check.
 *
 * 运行 / Run: tools/run_vocab_progress_host_test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// 可控的时钟。/ A controllable clock.
static time_t g_now;
static time_t fake_time(time_t *t) {
    if (t) *t = g_now;
    return g_now;
}
#define time(x) fake_time(x)

// 词库由测试提供，进度文件放在当前目录（测试先 chdir 进临时目录）。
// The test supplies the book; progress files live in the current directory (a temp dir).
static const uint8_t *g_blob, *g_blob_end;
#define VOCAB_BIN_START g_blob
#define VOCAB_BIN_END g_blob_end
#define VOCAB_FLASH_DIR "."
#include "../main/apps/vocab_book.c"

static int g_mount_fail;
esp_err_t book_store_flash_mount(void) { return g_mount_fail ? ESP_FAIL : ESP_OK; }

#define DAT "./vocab.dat"
#define TMP "./vocab.tmp"
#define BAK "./vocab.bak"
#define OLD "./vocab.old"

// ---------------------------------------------------------------- helpers

typedef struct { uint8_t *data; size_t size; } blob_t;

// 一个阶段、一册、一组，词按给定顺序。/ One stage, one book, one list, words in the given order.
static blob_t make_book(const char *const *words, int n) {
    static char strs[65536];
    size_t sl = 0;
    const uint32_t name_stage = (uint32_t)sl; sl += (size_t)snprintf(strs + sl, sizeof(strs) - sl, "Stage") + 1;
    const uint32_t name_book = (uint32_t)sl; sl += (size_t)snprintf(strs + sl, sizeof(strs) - sl, "Book") + 1;
    const uint32_t name_list = (uint32_t)sl; sl += (size_t)snprintf(strs + sl, sizeof(strs) - sl, "List") + 1;
    uint32_t woff[64];
    assert(n <= 64);
    for (int i = 0; i < n; ++i) {
        woff[i] = (uint32_t)sl;
        sl += (size_t)snprintf(strs + sl, sizeof(strs) - sl, "%s", words[i]) + 1;
        sl += (size_t)snprintf(strs + sl, sizeof(strs) - sl, "ipa") + 1;
        sl += (size_t)snprintf(strs + sl, sizeof(strs) - sl, "n. meaning-%s", words[i]) + 1;
        sl += (size_t)snprintf(strs + sl, sizeof(strs) - sl, "n") + 1;
        for (int k = 0; k < 3; ++k) strs[sl++] = 0;
    }
    const uint32_t off_stage = 40, off_book = 48, off_list = 56, off_ids = 68;
    const uint32_t off_word = (off_ids + 2u * (uint32_t)n + 3u) & ~3u, off_str = off_word + 4u * (uint32_t)n;
    blob_t b = {.size = off_str + sl};
    b.data = calloc(1, b.size);
    const vocab_head_t h = {.magic = {'V', 'O', 'C', '2'}, .stages = 1, .books = 1, .lists = 1, .words = (uint16_t)n,
                            .ids = (uint32_t)n, .off_stage = off_stage, .off_book = off_book, .off_list = off_list,
                            .off_ids = off_ids, .off_word = off_word, .off_str = off_str};
    const vocab_group_t stage = {name_stage, 0, 1}, book = {name_book, 0, 1};
    const vocab_list_t list = {name_list, 0, (uint16_t)n, 0};
    memcpy(b.data, &h, sizeof(h));
    memcpy(b.data + off_stage, &stage, sizeof(stage));
    memcpy(b.data + off_book, &book, sizeof(book));
    memcpy(b.data + off_list, &list, sizeof(list));
    for (int i = 0; i < n; ++i) {
        const uint16_t id = (uint16_t)i;
        memcpy(b.data + off_ids + 2 * i, &id, 2);
        memcpy(b.data + off_word + 4 * i, &woff[i], 4);
    }
    memcpy(b.data + off_str, strs, sl);
    return b;
}

static void close_book(void) {
    free(s_prog);
    s_prog = NULL;
    s_head = NULL;
    s_broken = s_dirty = s_unloaded = false;
    s_daily = k_daily_fresh;
    s_last_day = 0;
    s_dir = VOCAB_DIR_MIXED;
}

static void open_book(const blob_t *b) {
    close_book();
    g_blob = b->data;
    g_blob_end = b->data + b->size;
    assert(vocab_open());
}

static int id_of(const char *en) {
    for (int i = 0; i < vocab_word_count(); ++i) {
        vocab_word_t w;
        assert(vocab_word(i, &w));
        if (!strcmp(w.en, en)) return i;
    }
    return -1;
}

static long fsize(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static uint8_t *slurp(const char *path, long *size) {
    *size = fsize(path);
    assert(*size >= 0);
    FILE *f = fopen(path, "rb");
    assert(f);
    uint8_t *buf = malloc((size_t)*size + 1);
    assert(fread(buf, 1, (size_t)*size, f) == (size_t)*size);
    fclose(f);
    return buf;
}

static void wipe(void) {
    remove(DAT);
    remove(TMP);
    remove(BAK);
    remove(OLD);
}

static char magic_of(const char *path) {
    long n;
    uint8_t *b = slurp(path, &n);
    assert(n >= 4 && !memcmp(b, "VPR", 3));
    const char m = (char)b[3];
    free(b);
    return m;
}

static long vpr5_size(int records) {
    return (long)(sizeof(progress_head_t) + sizeof(daily_t) + sizeof(uint16_t) + (size_t)records * sizeof(progress_rec_t));
}

static const uint16_t D = 20400;   // 2025-11-08 左右 / about 2025-11-08
static time_t noon(uint16_t day) { return (time_t)day * 86400 - 8 * 3600 + 12 * 3600; }

static vocab_progress_t prog(uint8_t level, uint8_t seen, uint8_t wrong, uint16_t due) {
    return (vocab_progress_t){.level = level, .seen = seen, .wrong = wrong, .due = due, .touch = D, .right = D};
}

// 写一个旧版 VPR4（全部词）给当前打开的词库。/ Write an old VPR4 file (every word) for the open book.
static void write_vpr4(const char *path, uint32_t sig) {
    FILE *f = fopen(path, "wb");
    assert(f);
    const progress_head_t h = {.magic = {'V', 'P', 'R', '4'}, .book_sig = sig, .words = s_head->words,
                               .scope_first = 0, .scope_count = 1, .last_day = D};
    daily_t d = k_daily_fresh;
    d.daily_new = 10;
    d.streak = 7;
    d.study_day = D;
    assert(fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&d, sizeof(d), 1, f) == 1);
    for (int i = 0; i < s_head->words; ++i) {
        const uint32_t hash = word_hash(str(s_word[i]));
        assert(fwrite(&hash, sizeof(hash), 1, f) == 1);
    }
    assert(fwrite(s_prog, sizeof(*s_prog), s_head->words, f) == s_head->words);
    fclose(f);
}

static void write_vpr3(const char *path, uint32_t sig) {
    FILE *f = fopen(path, "wb");
    assert(f);
    const progress_head_t h = {.magic = {'V', 'P', 'R', '3'}, .book_sig = sig, .words = s_head->words,
                               .scope_first = 0, .scope_count = 1, .last_day = D};
    const daily_t d = k_daily_fresh;
    assert(fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&d, sizeof(d), 1, f) == 1);
    for (int i = 0; i < s_head->words; ++i) {
        const vocab_progress_t *p = &s_prog[i];
        const vocab_progress_v3_t o = {p->level, p->seen, p->wrong, p->streak, p->due, p->touch};
        assert(fwrite(&o, sizeof(o), 1, f) == 1);
    }
    fclose(f);
}

static const char *const k_book_a[] = {"apple", "banana", "cherry", "date", "egg", "fig", "grape", "hat", "ice", "jam"};
static const char *const k_book_b[] = {"kiwi", "apple", "banana", "cherry", "date", "egg", "grape", "hat", "ice", "jam", "lamp"};

// ---------------------------------------------------------------- tests

static void test_round_trip(const blob_t *a) {
    wipe();
    g_now = noon(D);
    open_book(a);
    for (int i = 0; i < s_head->words; ++i) assert(!touched(&s_prog[i]) && s_prog[i].right == NO_DAY);
    today();
    s_prog[id_of("cherry")] = prog(3, 4, 1, D + 4);
    s_prog[id_of("grape")] = prog(1, 1, 0, D + 1);
    s_daily.new_today = 2;
    s_scope = (vocab_scope_t){0, 1};
    s_dir = VOCAB_DIR_ZH_EN;
    s_dirty = true;
    vocab_save();
    assert(!s_dirty);
    // 只有两条记录，没有 tmp，也没有 bak（之前没有 dat）。/ Two records only; no tmp, and no bak (no earlier dat).
    assert(fsize(DAT) == vpr5_size(2) && magic_of(DAT) == '5' && fsize(TMP) < 0 && fsize(BAK) < 0);
    vocab_progress_t saved[16];
    memcpy(saved, s_prog, sizeof(*s_prog) * s_head->words);
    open_book(a);
    assert(!memcmp(saved, s_prog, sizeof(*s_prog) * s_head->words));
    assert(s_last_day == D && s_daily.new_today == 2 && s_dir == VOCAB_DIR_ZH_EN && s_scope.count == 1 && !s_dirty);
    puts("ok  fresh save/load round trip, touched-only records");
}

static void test_bak_rotation_and_recovery(const blob_t *a) {
    // 承接上一个测试：dat 里 cherry 是 3 级。/ Continues from the round trip: cherry is level 3 in dat.
    const int cherry = id_of("cherry");
    s_prog[cherry].level = 4;
    s_dirty = true;
    vocab_save();
    assert(fsize(DAT) > 0 && fsize(BAK) > 0 && fsize(TMP) < 0);   // bak 留着当上一版 / bak kept as the previous one
    // dat 在时旧 bak 不会赢。/ A stale bak never wins over dat.
    open_book(a);
    assert(s_prog[cherry].level == 4);

    // 掉电在 dat→bak 之后、tmp→dat 之前：dat 没了，tmp 是新的。/ Power loss after dat→bak, before tmp→dat.
    assert(rename(DAT, TMP) == 0);
    open_book(a);
    assert(s_prog[cherry].level == 4 && fsize(DAT) > 0 && fsize(TMP) < 0 && fsize(BAK) > 0);

    // dat 和 tmp 都没有：用 bak（上一版，3 级）。/ Neither dat nor tmp: fall back to bak (level 3).
    remove(DAT);
    open_book(a);
    assert(s_prog[cherry].level == 3 && fsize(DAT) > 0 && fsize(BAK) < 0);
    puts("ok  bak rotation, stale bak loses to dat, recovery from tmp and from bak");
}

static void test_damaged_set_aside(const blob_t *a) {
    wipe();
    g_now = noon(D);
    open_book(a);
    today();
    s_prog[id_of("egg")] = prog(2, 2, 0, D + 2);
    s_dirty = true;
    vocab_save();
    // 截断 dat：挪成 vocab.old，从头开始，之后的保存不碰它。/ Truncated dat: kept as vocab.old, fresh start.
    assert(truncate(DAT, fsize(DAT) - 5) == 0);
    long bad_size;
    uint8_t *bad = slurp(DAT, &bad_size);
    open_book(a);
    assert(fsize(DAT) < 0 && fsize(OLD) == bad_size);
    for (int i = 0; i < s_head->words; ++i) assert(!touched(&s_prog[i]));
    s_prog[id_of("jam")] = prog(1, 1, 1, D);
    s_dirty = true;
    vocab_save();
    long now_size;
    uint8_t *kept = slurp(OLD, &now_size);
    assert(now_size == bad_size && !memcmp(kept, bad, (size_t)bad_size) && fsize(DAT) == vpr5_size(1));
    free(kept);
    free(bad);

    // 再坏一个（魔数错）：vocab.old 已有，只留第一份；有 bak 就从 bak 恢复。
    // A second damaged dat (bad magic): the first vocab.old stays; bak recovers progress.
    s_prog[id_of("jam")].level = 2;
    s_dirty = true;
    vocab_save();   // dat: jam 2 级，bak: jam 1 级 / dat: jam level 2, bak: jam level 1
    FILE *f = fopen(DAT, "r+b");
    assert(f && fwrite("XXXX", 4, 1, f) == 1);
    fclose(f);
    open_book(a);
    assert(s_prog[id_of("jam")].level == 1 && fsize(OLD) == bad_size && fsize(DAT) > 0);
    puts("ok  truncated or corrupt dat set aside once and never overwritten");
}

static void test_vpr4_migration(const blob_t *a) {
    wipe();
    g_now = noon(D);
    open_book(a);
    s_prog[id_of("banana")] = prog(5, 9, 2, D + 15);
    s_prog[id_of("ice")] = prog(2, 3, 0, D + 2);
    vocab_progress_t saved[16];
    memcpy(saved, s_prog, sizeof(*s_prog) * s_head->words);
    write_vpr4(DAT, s_sig);
    open_book(a);
    assert(!memcmp(saved, s_prog, sizeof(*s_prog) * s_head->words));
    assert(s_daily.daily_new == 10 && s_daily.streak == 7 && s_dirty);
    vocab_save();
    assert(magic_of(DAT) == '5' && fsize(DAT) == vpr5_size(2));
    open_book(a);
    assert(!memcmp(saved, s_prog, sizeof(*s_prog) * s_head->words));
    puts("ok  VPR4 migration (same book) rewrites as sparse VPR5");
}

static void test_remap(const blob_t *a, const blob_t *b) {
    // VPR5：词库 A 学了 cherry、grape、fig；词库 B 去掉 fig、开头加 kiwi，id 全部移位。
    // VPR5: cherry, grape and fig learned in book A; book B drops fig and prepends kiwi, shifting every id.
    wipe();
    g_now = noon(D);
    open_book(a);
    today();
    s_prog[id_of("cherry")] = prog(4, 5, 1, D + 7);
    s_prog[id_of("grape")] = prog(2, 2, 0, D + 2);
    s_prog[id_of("fig")] = prog(1, 1, 1, D);
    const vocab_progress_t cherry = s_prog[id_of("cherry")], grape = s_prog[id_of("grape")];
    s_scope = (vocab_scope_t){0, 1};
    s_dirty = true;
    vocab_save();
    open_book(b);
    assert(id_of("cherry") == 3 && id_of("fig") < 0);
    assert(!memcmp(&s_prog[id_of("cherry")], &cherry, sizeof(cherry)) && !memcmp(&s_prog[id_of("grape")], &grape, sizeof(grape)));
    assert(!touched(&s_prog[id_of("kiwi")]) && !touched(&s_prog[id_of("lamp")]) && !touched(&s_prog[id_of("apple")]));
    assert(s_dirty);   // 词库变了要重写 / a changed book is rewritten
    vocab_save();
    assert(fsize(DAT) == vpr5_size(2));
    open_book(b);
    assert(!s_dirty && !memcmp(&s_prog[id_of("cherry")], &cherry, sizeof(cherry)));

    // VPR4 从别的词库：按文件里的哈希对回。/ VPR4 from another book: matched through its stored hashes.
    wipe();
    open_book(a);
    s_prog[id_of("date")] = prog(3, 3, 0, D + 4);
    s_prog[id_of("fig")] = prog(2, 2, 0, D + 2);
    const vocab_progress_t date = s_prog[id_of("date")];
    write_vpr4(DAT, s_sig);
    open_book(b);
    assert(!memcmp(&s_prog[id_of("date")], &date, sizeof(date)));
    int n = 0;
    for (int i = 0; i < s_head->words; ++i) n += touched(&s_prog[i]);
    assert(n == 1 && s_dirty);
    puts("ok  remap by headword hash after a word is added and removed (VPR5 and VPR4)");
}

static void test_vpr3(const blob_t *a, const blob_t *b) {
    // 同一词库的 VPR3 照常迁移。/ A VPR3 file from the same book migrates.
    wipe();
    open_book(a);
    s_prog[id_of("hat")] = prog(2, 2, 0, D + 2);
    write_vpr3(DAT, s_sig);
    open_book(a);
    assert(s_prog[id_of("hat")].level == 2 && s_prog[id_of("hat")].right == NO_DAY && s_dirty);
    // 签名对不上的 VPR3 挪开、从头开始。/ A VPR3 file from another book is set aside, fresh start.
    open_book(a);
    write_vpr3(DAT, s_sig);
    remove(BAK);
    open_book(b);
    assert(fsize(DAT) < 0 && fsize(OLD) > 0);
    for (int i = 0; i < s_head->words; ++i) assert(!touched(&s_prog[i]));
    puts("ok  VPR3 migrates for the same book, is set aside for another");
}

static void test_unreadable_mount(const blob_t *a) {
    // 打开时挂载失败：之后保存前先补读，不会用空进度盖掉文件。
    // Mount fails at open: the save reads the file first and never overwrites it with empty progress.
    wipe();
    g_now = noon(D);
    open_book(a);
    today();
    s_prog[id_of("apple")] = prog(4, 6, 0, D + 7);
    s_dirty = true;
    vocab_save();
    g_mount_fail = 1;
    open_book(a);
    assert(s_unloaded && !touched(&s_prog[id_of("apple")]));
    today();
    vocab_save();   // 挂不上，不写 / cannot mount, nothing written
    g_mount_fail = 0;
    vocab_save();
    assert(!s_unloaded && s_prog[id_of("apple")].level == 4);
    open_book(a);
    assert(s_prog[id_of("apple")].level == 4);
    puts("ok  progress unread at open is re-read before saving, never overwritten");
}

static void test_day_forward(const blob_t *a) {
    wipe();
    open_book(a);
    g_now = noon(D);
    assert(today() == D);
    s_daily.new_today = 3;
    g_now = noon(D - 5);   // 往回拨 5 天：日子和计数不变 / 5 days back: day and counters kept
    assert(today() == D && s_daily.new_today == 3);
    g_now = 1000;          // 没对时：沿用 / unsynced: keep the day
    assert(today() == D && s_daily.new_today == 3);
    g_now = noon(D + 1);   // 新的一天：清零 / a new day resets the counters
    assert(today() == D + 1 && s_daily.new_today == 0);
    s_daily.new_today = 2;
    g_now = noon(D + 1 - 40);   // 往回超过一个月：当作时钟被纠正 / over a month back: a corrected clock
    assert(today() == D - 39 && s_daily.new_today == 0);

    // 没有进度文件时第一次答题也算打卡。/ With no progress file the first answer still counts toward the streak.
    wipe();
    open_book(a);
    g_now = noon(D);
    assert(vocab_round_start(VOCAB_ROUND_SCOPE) > 0);
    vocab_round_answer(VOCAB_KNOWN);
    assert(vocab_streak() == 1);
    puts("ok  the day only moves forward; streak counts without a progress file");
}

static void test_book_check(const char *real_path) {
    long n;
    uint8_t *real = slurp(real_path, &n);
    assert(book_valid(real, (size_t)n));
    // 一个越界的 id、一个越界的词偏移、末尾不是 NUL，都不通过。/ An id, a word offset or a missing final NUL fails.
    const vocab_head_t *h = (const vocab_head_t *)real;
    uint16_t id;
    memcpy(&id, real + h->off_ids, 2);
    const uint16_t bad_id = h->words;
    memcpy(real + h->off_ids, &bad_id, 2);
    assert(!book_valid(real, (size_t)n));
    memcpy(real + h->off_ids, &id, 2);
    uint32_t off;
    memcpy(&off, real + h->off_word, 4);
    const uint32_t bad_off = (uint32_t)n - h->off_str;
    memcpy(real + h->off_word, &bad_off, 4);
    assert(!book_valid(real, (size_t)n));
    memcpy(real + h->off_word, &off, 4);
    real[n - 1] = 'x';
    assert(!book_valid(real, (size_t)n));
    real[n - 1] = 0;
    assert(book_valid(real, (size_t)n));
    // 真实词库能打开，每个词七个字段都在字符串区内。/ The real book opens, every field inside the strings.
    blob_t b = {real, (size_t)n};
    wipe();
    open_book(&b);
    for (int i = 0; i < s_head->words; ++i) {
        vocab_word_t w;
        assert(vocab_word(i, &w) && w.en[0] && w.ex_zh >= s_str && w.ex_zh < s_str_end + 1);
    }
    printf("ok  built-in book check (%d words) and damaged copies rejected\n", s_head->words);
    close_book();
    free(real);
}

int main(int argc, char **argv) {
    assert(argc == 2);   // 真实 vocab.bin 的路径 / path to the real vocab.bin
    char real[1024];
    assert(realpath(argv[1], real));
    char dir[] = "/tmp/vocab-progress-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    blob_t a = make_book(k_book_a, (int)(sizeof(k_book_a) / sizeof(k_book_a[0])));
    blob_t b = make_book(k_book_b, (int)(sizeof(k_book_b) / sizeof(k_book_b[0])));
    test_round_trip(&a);
    test_bak_rotation_and_recovery(&a);
    test_damaged_set_aside(&a);
    test_vpr4_migration(&a);
    test_remap(&a, &b);
    test_vpr3(&a, &b);
    test_unreadable_mount(&a);
    test_day_forward(&a);
    test_book_check(real);
    wipe();
    assert(chdir("/") == 0 && rmdir(dir) == 0);
    free(a.data);
    free(b.data);
    puts("PASS: vocab progress host test");
    return 0;
}
