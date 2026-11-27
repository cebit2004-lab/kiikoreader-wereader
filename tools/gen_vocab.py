#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：生成背单词内置词库 main/assets/vocab.bin。来源：人教版小学英语单元词汇表（vocab_extract_pep.py 输出）、
#       ECDICT（MIT，https://github.com/skywind3000/ECDICT）的中考词汇标签与词频。
# English: Build the built-in vocab book main/assets/vocab.bin from the PEP primary unit word lists
#          (vocab_extract_pep.py output) and ECDICT (MIT) for the zhongkao tag and word frequency.
#
# 用法 / Usage:
#   python3 tools/gen_vocab.py --dump-charset build/vocab_charset.txt      # 需要 fontTools / needs fontTools
#   python3 -I tools/gen_vocab.py --pep pep.json --ecdict ecdict.csv --charset build/vocab_charset.txt \
#           --out main/assets/vocab.bin
#
# 格式（小端）/ Format (little endian):
#   header: 'VOC2', u16 stages, books, lists, words, u32 ids, u32 off_stage, off_book, off_list, off_ids, off_word, off_str
#   stage:  u32 name, u16 first_book, u16 books        book: u32 name, u16 first_list, u16 lists
#   list:   u32 name, u32 first_id, u16 count, u16 0   ids: u16 word index
#   word:   u32 -> "en\0ipa\0zh\0pos\0forms\0example_en\0example_zh\0"
#   例句来自 Tatoeba（CC BY 2.0 FR）中英对照；词形来自 ECDICT exchange 字段与派生词校验。
#   Examples come from Tatoeba (CC BY 2.0 FR) English–Chinese pairs; forms from ECDICT's exchange field
#   plus checked derivations.
import argparse
import csv
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / 'boards/metalio47/src/main/assets/builtin.ttf'
HANZI = ROOT / 'boards/metalio47/src/main/assets/ui-hanzi.bin'
TERMS = [(3, '上'), (3, '下'), (4, '上'), (4, '下'), (5, '上'), (5, '下'), (6, '上'), (6, '下')]
GRADE = {3: '三', 4: '四', 5: '五', 6: '六'}
ZH_MAX = 60     # UTF-8 字节上限，约 20 个汉字 / UTF-8 byte cap, about 20 Han characters
COMMON_N = 2000


def dump_charset(path):
    from fontTools.ttLib import TTFont
    cps = set(TTFont(FONT).getBestCmap())
    data = HANZI.read_bytes()
    count = struct.unpack_from('<I', data, 4)[0]
    cps |= set(struct.unpack_from('<' + 'H' * count, data, 16))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(''.join(chr(c) for c in sorted(cps) if c >= 0x20), encoding='utf-8')


def norm_ecdict_ipa(s):
    s = s.strip().replace("'", 'ˈ').replace(',', 'ˌ').replace('.', 'ˌ').replace(':', 'ː')
    s = s.replace('ә', 'ə').replace('є', 'e').replace('^', 'ʌ').replace('\\', '')
    return re.sub(r'\s+', ' ', s)


def short_zh(translation):
    lines = []
    for line in re.split(r'\\n|\n', translation.replace('\r', '')):
        line = line.strip()
        if not line or line.startswith('['): continue
        # 每个词性只保留前三个义项 / Keep the first three senses per part of speech.
        m = re.match(r'^([a-z]+\.)\s*(.*)$', line)
        pos, body = (m.group(1) + ' ', m.group(2)) if m else ('', line)
        senses = [x.strip() for x in re.split(r'[,，;；]', body) if x.strip()]
        lines.append(pos + '，'.join(senses[:3]))
        if len(lines) == 2: break
    return '；'.join(lines)


def clip_utf8(s, limit):
    out = ''
    for ch in s:
        if len((out + ch).encode()) > limit: return out.rstrip('，；,; ') + '…'
        out += ch
    return out


POS_NAMES = [('pron.', 'pron', '代词'), ('prep.', 'prep', '介词'), ('conj.', 'conj', '连词'), ('adv.', 'adv', '副词'),
             ('num.', 'num', '数词'), ('int.', 'int', '感叹词'), ('interj.', 'int', '感叹词'), ('art.', 'art', '冠词'),
             ('aux.', 'v', '动词'), ('vt.', 'v', '动词'), ('vi.', 'v', '动词'), ('v.', 'v', '动词'),
             ('adj.', 'a', '形容词'), ('a.', 'a', '形容词'), ('n.', 'n', '名词')]
POS_ZH = {code: zh for _, code, zh in POS_NAMES}
EXCHANGE = [('s', '复数'), ('p', '过去式'), ('d', '过去分词'), ('i', '现在分词'), ('3', '三单'),
            ('r', '比较级'), ('t', '最高级')]
PRONOUNS = {
    'i': '宾格 me · 物主 my / mine · 反身 myself', 'you': '宾格 you · 物主 your / yours · 反身 yourself / yourselves',
    'he': '宾格 him · 物主 his · 反身 himself', 'she': '宾格 her · 物主 her / hers · 反身 herself',
    'it': '宾格 it · 物主 its · 反身 itself', 'we': '宾格 us · 物主 our / ours · 反身 ourselves',
    'they': '宾格 them · 物主 their / theirs · 反身 themselves',
    'me': '主格 I · 物主 my / mine', 'him': '主格 he · 物主 his', 'her': '主格 she · 物主 her / hers',
    'us': '主格 we · 物主 our / ours', 'them': '主格 they · 物主 their / theirs',
    'my': '主格 I · 名词性 mine', 'your': '主格 you · 名词性 yours', 'his': '主格 he · 名词性 his',
    'our': '主格 we · 名词性 ours', 'their': '主格 they · 名词性 theirs',
}
HAN = re.compile(r'[\u4e00-\u9fff]')


def pos_codes(translation):
    codes = []
    for line in re.split(r'\\n|\n', translation.replace('\r', '')):
        line = line.strip()
        for prefix, code, _ in POS_NAMES:
            if line.startswith(prefix):
                if code not in codes: codes.append(code)
                break
    return codes


def enrich_words(words, ec, pairs, easy_rank):
    """返回每个词的 (pos, forms, ex_en, ex_zh)。/ Return (pos, forms, ex_en, ex_zh) for every word."""
    # 例句索引：词 -> 句子编号。/ Example index: token -> pair numbers.
    tok = re.compile(r"[a-z]+(?:'[a-z]+)?")
    index = {}
    toks = []
    for n, (e, _) in enumerate(pairs):
        t = tok.findall(e.lower())
        toks.append(t)
        for w in set(t): index.setdefault(w, []).append(n)

    def han(text): return set(HAN.findall(text))

    def zh_of(key):
        r = ec.get(key)
        return r['translation'] if r else ''

    def related(base, base_zh, cand, want):
        r = ec.get(cand)
        if not r or cand == base or want not in pos_codes(r['translation']): return False
        rank = int(r['frq'] or 0)
        if rank == 0 or rank > 30000 or len(index.get(cand, ())) < 2: return False   # 生僻派生词不列 / skip rare derivations
        return bool(han(base_zh) & han(short_zh(r['translation'])))

    def derived(w, codes, base_zh):
        out = []
        def tryit(label, want, cands):
            for c in cands:
                if related(w, base_zh, c, want):
                    out.append(f'{label} {c}')
                    return
        dbl = w + w[-1] if re.search(r'[^aeiou][aeiou][bdgmnprt]$', w) else w
        if 'a' in codes:
            tryit('副词', 'adv', [w + 'ly', w[:-1] + 'ily' if w.endswith('y') else '', w[:-2] + 'ly' if w.endswith('le') else '',
                               w + 'ally' if w.endswith('ic') else '', w[:-1] + 'ly' if w.endswith('e') else ''])
            tryit('名词', 'n', [w + 'ness', w[:-1] + 'iness' if w.endswith('y') else ''])
        if 'adv' in codes and w.endswith('ly'):
            tryit('形容词', 'a', [w[:-2], w[:-3] + 'y' if w.endswith('ily') else '', w[:-2] + 'e', w[:-1] + 'e' if w.endswith('bly') else ''])
        if 'n' in codes and 'a' not in codes:
            tryit('形容词', 'a', [w + 'y', dbl + 'y', w + 'ful', w + 'al', w + 'ous', w + 'ly', w[:-1] + 'y' if w.endswith('e') else ''])
        if 'v' in codes:
            tryit('名词', 'n', [w + 'er', w + 'r' if w.endswith('e') else '', dbl + 'er', w + 'or', w + 'ment', w + 'tion',
                              w[:-1] + 'ion' if w.endswith('e') else ''])
        return [x for x in out if x]

    def example(key):
        if not key: return '', ''
        r = ec.get(key)
        targets = {key}
        if r:
            for part in r['exchange'].split('/'):
                if ':' in part and part[0] in 'pdi3srt': targets.update(part[2:].lower().split(','))
        words_ = key.split()
        if len(words_) == 1:
            cand = set()
            for t in targets: cand.update(index.get(t, ()))
        else:
            sets = [set(index.get(w_, ())) for w_ in words_ if w_ not in ('…', '...')]
            cand = set.intersection(*sets) if sets else set()
            pat = re.compile(r'\b' + r'\s+'.join(re.escape(w_) if w_ not in ('…', '...') else r'\w+(?:\s+\w+)?'
                                                 for w_ in words_) + r'\b', re.I)
            cand = {n for n in cand if pat.search(pairs[n][0])}
        best, score = None, None
        for n in cand:
            e, z = pairs[n]
            t = toks[n]
            if len(e) > 72 or len(t) < 3 or len(z) > 36: continue
            sc = max(0, 4 - len(t)) * 2 + max(0, len(t) - 8) * 1.5 + len(z) / 30
            for w_ in t:
                if w_ in targets: continue
                rk = easy_rank.get(w_)
                if rk is None or rk > 3000: sc += 2.5
            sc += 1.5 * len(re.findall(r'(?<!^)\b[A-Z][a-z]+', e))   # 人名 / names
            if score is None or sc < score or (sc == score and n < best): best, score = n, sc
        return pairs[best] if best is not None else ('', '')

    out = []
    for en, _, zh in words:
        key = en.lower()
        r = ec.get(key)
        codes = pos_codes(r['translation']) if r and ' ' not in key else []
        if ' ' in key: codes = ['phr']
        # 主词性：与显示的释义共用汉字最多的那一行。/ Main POS: the line sharing most Han with the shown meaning.
        main_pos = codes[0] if codes else ''
        if r and len(codes) > 1:
            best, tied, touched = -1, [], set()
            for line in re.split(r'\\n|\n', r['translation']):
                lc = pos_codes(line)
                if not lc: continue
                n = len(han(zh) & han(line))
                if n: touched.add(lc[0])
                if n > best: best, main_pos, tied = n, lc[0], [lc[0]]
                elif n == best: tied.append(lc[0])
            # 名/动并列时看过去式是否常用（ran 常用、rabbited 不常用）；以「的」结尾的释义偏形容词。
            # On a noun/verb tie, a common past form means verb (ran vs rabbited); "…的" meanings lean adjective.
            past = dict(x.split(':', 1) for x in r['exchange'].split('/') if ':' in x).get('p', '')
            if 'n' in tied and 'v' in tied:
                main_pos = 'v' if past and len(index.get(past.lower(), ())) >= 10 else 'n'
            if 'a' in tied and zh.rstrip('）').endswith('的'): main_pos = 'a'
            codes = [main_pos] + [c for c in codes if c != main_pos]
        else:
            touched = set()
        ex = dict(x.split(':', 1) for x in r['exchange'].split('/') if ':' in x) if r else {}

        def valid(k):
            form = ex.get(k, '').lower()
            back = ec.get(form)
            # 变形须指回原词（ECDICT 偶有 fee→feed 这类错配）。/ A form must point back to its lemma.
            return bool(form) and (form == key or not back or f'0:{key}' in back['exchange'].split('/'))

        def common(k): return valid(k) and len(index.get(ex[k].lower(), ())) >= 3

        # 释义也涉及的其他词性，在变形常用时一并给出（run 的 ran、water 的 waters）。
        # Other parts of speech the meaning also covers get their forms when those forms are common.
        # 主词性是动词时不再列名词复数（swims 这类没有意义）。/ No plural when the main POS is a verb.
        shown = {main_pos} | {c for c in touched if c == 'v' and common('p') or
                              c == 'n' and main_pos != 'v' and common('s') or c in ('a', 'adv') and common('r')}
        forms = []
        if key in PRONOUNS: forms.append(PRONOUNS[key])
        elif r and ' ' not in key:
            for k, label in EXCHANGE:
                if not valid(k): continue
                # put/cut 这类过去式与原形相同也要列。/ List pasts equal to the base too (put, cut).
                if ex[k].lower() != key or k in 'pd':
                    if k == 's' and 'n' not in shown: continue
                    if k in 'pdi3' and 'v' not in shown: continue
                    if k in 'rt' and not ({'a', 'adv'} & shown): continue
                    forms.append(f'{label} {ex[k]}')
            forms += derived(key, codes, zh + zh_of(key))
        e, z = example(key)
        out.append((' '.join(codes[:2]), ' · '.join(forms), e, z))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump-charset')
    ap.add_argument('--pep'); ap.add_argument('--ecdict'); ap.add_argument('--charset'); ap.add_argument('--out'); ap.add_argument('--pairs')
    a = ap.parse_args()
    if a.dump_charset: return dump_charset(a.dump_charset)

    allowed = set(Path(a.charset).read_text(encoding='utf-8'))
    csv.field_size_limit(1 << 30)
    ec, freq, zk = {}, [], []
    with open(a.ecdict, encoding='utf-8', newline='') as f:
        for r in csv.DictReader(f):
            w = r['word']
            key = w.lower()
            if key not in ec or w == key: ec[key] = r
            if 'zk' in r['tag'].split() and re.fullmatch(r"[A-Za-z][A-Za-z'\- ]*", w): zk.append(w)
            fr = int(r['frq'] or 0)
            if fr > 0 and re.fullmatch(r'[a-z]+', w) and r['translation'] and len(w) > 1 or w in ('a', 'i'):
                if not any(p.startswith('0:') for p in r['exchange'].split('/')): freq.append((fr, w))

    dropped = set()

    def clean(s):
        s = s.replace('⋯', '…').replace('……', '…').replace('...', '…')
        out = ''.join(ch if ch in allowed else '' for ch in s)
        dropped.update(ch for ch in s if ch not in allowed)
        return out

    words, index = [], {}

    def word_id(en, ipa, zh):
        key = en.lower()
        if key in index: return index[key]
        r = ec.get(key)
        if not ipa and r and ' ' not in en: ipa = norm_ecdict_ipa(r['phonetic'])
        if not zh and r: zh = short_zh(r['translation'])
        if not zh: return None
        index[key] = len(words)
        words.append((clean(en), clean(ipa), clip_utf8(clean(zh), ZH_MAX)))
        return index[key]

    stages, books, lists, ids = [], [], [], []

    def add_list(name, members):
        got = [i for i in members if i is not None]
        if not got: return
        lists.append((name, len(ids), len(got)))
        ids.extend(got)

    # 小学：人教版 PEP 三年级起点，按册、单元。/ Primary: PEP, per term and unit.
    pep = json.loads(Path(a.pep).read_text(encoding='utf-8'))
    first_book = len(books)
    for g, t in TERMS:
        items = pep.get(f'{g}{t}', [])
        first_list = len(lists)
        for unit in sorted({i['unit'] for i in items}):
            members = []
            for it in items:
                if it['unit'] != unit: continue
                en = re.sub(r"\s*'\s*", "'", it['en']).replace('’', "'")
                zh = it['zh'].lstrip('；;，, ')
                if en.endswith(' T') and zh.startswith('恤'): en, zh = en[:-2], 'T' + zh
                ipas = [x.strip('/ ') for x in re.findall(r'/[^/]+/', it['ipa'])]
                pairs = re.findall(r'([a-z]+)([\u4e00-\u9fff]+)', zh)
                if en == 'Numbers' and len(pairs) == len(ipas):
                    # 数词表在课本里排成一块，没有逐词页码 / The numbers table is one block without per-word pages.
                    members.extend(word_id(w, i, z) for (w, z), i in zip(pairs, ipas))
                    continue
                members.append(word_id(en, ' '.join(ipas), zh))
            add_list(f'Unit {unit}', members)
        books.append((f'{GRADE[g]}年级{t}册', first_list, len(lists) - first_list))
    stages.append(('小学', first_book, len(books) - first_book))

    # 初中：ECDICT 中考词汇，按字母每 50 个一组。/ Junior high: ECDICT zhongkao words, 50 per group by letter.
    zk_ids = [word_id(w, '', '') for w in sorted(set(zk), key=str.lower)]
    zk_ids = [i for i in zk_ids if i is not None]
    first_list = len(lists)
    for n in range(0, len(zk_ids), 50): add_list(f'第{n // 50 + 1}组', zk_ids[n:n + 50])
    books.append(('中考词汇', first_list, len(lists) - first_list))
    stages.append(('初中', len(books) - 1, 1))

    # 常用 2000 词：按词频，每 100 个一组。/ Common 2000: by frequency, 100 per group.
    freq.sort()
    common = []
    for _, w in freq:
        i = word_id(w, '', '')
        if i is not None and i not in common: common.append(i)
        if len(common) == COMMON_N: break
    first_list = len(lists)
    for n in range(0, len(common), 100): add_list(f'{n + 1}–{n + 100}', common[n:n + 100])
    books.append(('常用2000词', first_list, len(lists) - first_list))
    stages.append(('常用', len(books) - 1, 1))

    # 词性、词形与例句。/ Parts of speech, forms and examples.
    pairs = [tuple(l.rstrip('\n').split('\t', 1)) for l in open(a.pairs, encoding='utf-8') if '\t' in l]
    # 含屈折形式的词频排名（went、goes 也算常见）。/ Frequency ranks including inflected forms.
    easy_rank = {k: int(r['frq']) for k, r in ec.items() if (r['frq'] or '0') != '0'}
    for w in ('i', 'a', 'the', 'is', 'are', 'was', 'were', 'am', "don't", "it's", "i'm", "can't", "doesn't"):
        easy_rank.setdefault(w, 1)
    extra = enrich_words(words, ec, pairs, easy_rank)
    words = [(en, ipa, zh, clean(pos), clean(forms), clean(ee), clean(ez))
             for (en, ipa, zh), (pos, forms, ee, ez) in zip(words, extra)]
    print(f'examples {sum(1 for w in words if w[5])}/{len(words)}, forms {sum(1 for w in words if w[4])}',
          file=sys.stderr)

    # 进度按词头的 32 位 FNV-1a 哈希存（vocab_book.c），两个词头撞哈希会共用一条进度：生成时就拒绝。
    # Progress is keyed by the headword's 32-bit FNV-1a hash (vocab_book.c); two colliding headwords would
    # share one record, so refuse them here.
    def fnv(text):
        h = 2166136261
        for c in text.encode():
            h = ((h ^ c) * 16777619) & 0xffffffff
        return h
    seen_hash = {}
    for fields in words:
        other = seen_hash.setdefault(fnv(fields[0]), fields[0])
        if other != fields[0]:
            sys.exit(f'headword hash collision: {other!r} / {fields[0]!r}')

    # 打包 / Pack.
    strings = bytearray()
    pool = {}

    def s(text):
        if text not in pool:
            pool[text] = len(strings)
            strings.extend(text.encode() + b'\0')
        return pool[text]

    stage_b = b''.join(struct.pack('<IHH', s(n), f, c) for n, f, c in stages)
    book_b = b''.join(struct.pack('<IHH', s(n), f, c) for n, f, c in books)
    list_b = b''.join(struct.pack('<IIHH', s(n), f, c, 0) for n, f, c in lists)
    ids_b = struct.pack('<' + 'H' * len(ids), *ids)
    word_offs = []
    for fields in words:
        word_offs.append(len(strings))
        strings.extend(b''.join(f.encode() + b'\0' for f in fields))
    word_b = struct.pack('<' + 'I' * len(word_offs), *word_offs)
    head = 4 + 2 * 4 + 4 + 6 * 4
    offs = [head]
    for blob in (stage_b, book_b, list_b, ids_b, word_b):
        offs.append(offs[-1] + len(blob) + (-len(blob) % 4))
    out = bytearray(b'VOC2' + struct.pack('<HHHHI', len(stages), len(books), len(lists), len(words), len(ids)))
    out += struct.pack('<6I', *offs)
    for blob in (stage_b, book_b, list_b, ids_b, word_b):
        out += blob + b'\0' * (-len(blob) % 4)
    out += strings
    Path(a.out).write_bytes(out)
    print(f'stages {len(stages)} books {len(books)} lists {len(lists)} words {len(words)} ids {len(ids)} '
          f'bytes {len(out)}', file=sys.stderr)
    for n, f, c in books: print(' ', n, c, 'lists', sum(lists[f + k][2] for k in range(c)), 'words', file=sys.stderr)
    if dropped: print('dropped chars:', ''.join(sorted(dropped)), file=sys.stderr)


if __name__ == '__main__':
    main()
