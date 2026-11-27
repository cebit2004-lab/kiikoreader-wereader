#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：把国际音标字形从 Noto Sans（OFL）拷进内建界面字体，背单词页的音标才能显示。只追加缺的字形，原有字形不动。
# English: Copy IPA glyphs from Noto Sans (OFL) into the built-in UI face so the vocab page can show
# phonetics. Only missing glyphs are appended; existing glyphs stay byte-identical in shape.
import argparse
import copy as copymod
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# 英式词典音标常用字符，外加省略号。/ Characters used by British dictionary phonetics, plus the ellipsis.
IPA = 'ɑæɐəɚɛɜɝɪɒɔʊʌʃʒθðŋɡɹɾʔˈˌːˑɫʤʧɘɵɨʉ…'
TARGETS = ['boards/metalio47/src/main/assets/builtin.ttf']


def add(dst_path: Path, src):
    from fontTools.ttLib import TTFont
    dst = TTFont(dst_path)
    assert dst['head'].unitsPerEm == src['head'].unitsPerEm
    dcmap, scmap = dst.getBestCmap(), src.getBestCmap()
    have = set(dst.getGlyphOrder())
    sglyf, dglyf = src['glyf'], dst['glyf']
    renamed = {}

    def copy(name):
        if name in renamed: return renamed[name]
        new = 'ipa.' + name
        renamed[name] = new
        g = copymod.deepcopy(sglyf[name])
        if g.isComposite():
            for comp in g.components:
                comp.glyphName = copy(comp.glyphName)
        if new not in have:
            dglyf[new] = g
            dst['hmtx'][new] = src['hmtx'][name]
            have.add(new)
        return new

    added = []
    for ch in IPA:
        cp = ord(ch)
        if cp in dcmap: continue
        new = copy(scmap[cp])
        for table in dst['cmap'].tables:
            if table.isUnicode(): table.cmap[cp] = new
        added.append(ch)
    dst.setGlyphOrder(list(dglyf.glyphOrder))
    if 'post' in dst and dst['post'].formatType == 2.0:
        dst['post'].extraNames = []
    dst.save(dst_path)
    return ''.join(added)


def main():
    from fontTools.ttLib import TTFont
    p = argparse.ArgumentParser()
    p.add_argument('--src', type=Path, required=True, help='NotoSans-Regular.ttf (OFL)')
    args = p.parse_args()
    src = TTFont(args.src)
    for rel in TARGETS:
        print(rel, 'added', add(ROOT / rel, src) or 'nothing')


if __name__ == '__main__':
    main()
