#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：从人教版（PEP，三年级起点）英语课本 PDF 的「单元词汇表」抽出各单元单词与释义，生成 pep.json 供 gen_vocab.py 使用。
# English: Extract unit words and meanings from the "Words in each unit" pages of PEP English textbook PDFs
# into pep.json for gen_vocab.py. Usage: vocab_extract_pep.py <textbook dir> <out.json>
import fitz, re, glob, os, json, sys
D=sys.argv[1].rstrip('/')+'/' 
CN={'三':3,'四':4,'五':5,'六':6}
def book_key(name):
    g=CN[re.findall(r'([三四五六])\s*年级',name)[-1]]
    return g,('上' if '上册' in name else '下')
def page_range(doc):
    start=end=None
    for i in range(doc.page_count):
        t=doc[i].get_text().replace(' ','')
        if start is None:
            if 'Wordsineachunit' in t and '单元词汇表' in t and 'CONTENTS' not in t.upper(): start=i
        elif len(re.findall(r'p\.\d+',t))<3 or ('Vocabulary' in t and 'Unit' not in t):
            end=i;break
    return start,end or doc.page_count
IPA_FONT=lambda f:('Ipa' in f or 'SIL' in f or 'Cambria' in f or 'IPA' in f)
CJK=re.compile(r'[　-鿿＀-￯（）]')
def parse(path):
    doc=fitz.open(path); st,en=page_range(doc)
    unit=None; items=[]; buf=[]
    def finish(page):
        nonlocal buf
        en_parts=[];ipa=[];zh=[];bold=False
        for t,f in buf:
            t=t.replace('*','').replace('\t',' ').replace(' ',' ')
            if not t.strip(): continue
            if IPA_FONT(f):
                if 'Cambria' in f: ipa.append(t.strip())
                continue
            if CJK.search(t):
                m=re.match(r'\s*([A-Za-z][A-Za-z \-’\'.,?!]*?)\s*([　-鿿＀-￯（(].*)$',t)
                if m and not zh: en_parts.append(m.group(1)); zh.append(m.group(2))
                else: zh.append(t.strip())
            else:
                if zh: zh.append(t.strip())   # rare
                else:
                    en_parts.append(t.strip())
                    if 'Bold' in f: bold=True
        e=re.sub(r'\s+',' ',' '.join(en_parts)).strip()
        z=re.sub(r'\s+','',''.join(zh))
        if e and z: items.append(dict(unit=unit,en=e,zh=z,ipa=' '.join(ipa),bold=bold,page=page))
        elif e or z: print('  DROP',unit,repr(e),repr(z),file=sys.stderr)
        buf=[]
    for i in range(st,en):
        for b in doc[i].get_text("dict")["blocks"]:
            for l in b.get("lines",[]):
                for s in l["spans"]:
                    t=s["text"]
                    if not t.strip(): continue
                    m=re.match(r'\s*Unit\s*(\d+)\s*$',t)
                    if m and ('Bold' in s['font']) :
                        if buf: finish(None)
                        unit=int(m.group(1)); continue
                    if re.match(r'\s*Appendix',t):
                        continue
                    if '\u4ec5\u4f9b\u4e2a\u4eba\u5b66\u4e60' in t: continue
                    if unit is None: continue
                    pm=re.search(r'p\.\s*(\d+)\s*$',t.strip())
                    if pm:
                        body=t.strip()[:pm.start()]
                        if body.strip(): buf.append((body,s['font']))
                        finish(int(pm.group(1))); continue
                    if re.fullmatch(r'\s*\d+\s*',t): continue
                    buf.append((t,s['font']))
    if buf: finish(None)
    return (st,en),items
res={}
for f in sorted(glob.glob(D+'*英语*.pdf')):
    g,t=book_key(os.path.basename(f))
    rng,it=parse(f)
    res[f"{g}{t}"]=it
    from collections import Counter
    c=Counter(i['unit'] for i in it)
    print(g,t,rng,len(it),dict(sorted(c.items())),'bold',sum(i['bold'] for i in it),'ipa',sum(1 for i in it if i['ipa']))
json.dump(res,open(sys.argv[2],'w'),ensure_ascii=False,indent=0)
