#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：给背单词生成发音文件，拷到 SD 卡根目录的 vocab_audio/ 下即可点读，不联网。
#       读内置词库 main/assets/vocab.bin，每个词一个 w/<词>.mp3，有例句的再出一个 s/<词>.mp3。
#       文件名：词头小写，字母数字以外换成下划线（与 main/apps/app_vocab.c 的 audio_path 一致）。
#       输出 16 kHz 单声道 MP3（设备喇叭的时钟就是 16 kHz，不用重采样），前后静音已裁掉。
#       已有的文件跳过，可以中断后接着跑。
# English: Build pronunciation clips for the vocabulary trainer; copy them to vocab_audio/ at the root
#          of the SD card. Reads the built-in book main/assets/vocab.bin and writes w/<word>.mp3 per word
#          plus s/<word>.mp3 for words with an example. Names are the headword lower-cased with anything
#          but letters and digits as '_' (matching audio_path in main/apps/app_vocab.c). Output is
#          16 kHz mono MP3 (the speaker clock is 16 kHz, so no resampling) with silence trimmed.
#          Existing files are skipped, so an interrupted run can resume.
#
# 引擎 / Engines:
#   say   macOS 自带，离线（默认）。/ Built into macOS, offline (default).
#   edge  微软在线神经语音，音质更好，需要 `pip install edge-tts` 并联网。
#         Microsoft online neural voices, better quality; needs `pip install edge-tts` and a network.
# 都需要 ffmpeg（brew install ffmpeg）。/ Both need ffmpeg (brew install ffmpeg).
#
# 用法 / Usage:
#   python3 tools/gen_vocab_audio.py                                  # say，Samantha 美音 / US voice
#   python3 tools/gen_vocab_audio.py --voice Daniel                   # say，英音 / UK voice
#   python3 tools/gen_vocab_audio.py --engine edge --voice en-US-AriaNeural
#   python3 tools/gen_vocab_audio.py --words-only                     # 不生成例句 / skip examples
#   然后把 build/vocab_audio 整个拷到卡上，成为 /vocab_audio。
#   Then copy build/vocab_audio to the card so it becomes /vocab_audio.
import argparse
import concurrent.futures
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOOK = ROOT / 'main/assets/vocab.bin'
OUT = ROOT / 'build/vocab_audio'
DEFAULT_VOICE = {'say': 'Samantha', 'edge': 'en-US-AriaNeural'}
# 裁掉前后静音；喇叭前补的静音由设备自己加。/ Trim leading and trailing silence; the device pads its own lead-in.
TRIM = ('silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.03,'
        'areverse,silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.05,areverse')


def read_book(path):
    d = path.read_bytes()
    magic, _stages, _books, _lists, words, _ids = struct.unpack_from('<4sHHHHI', d, 0)
    if magic != b'VOC2':
        sys.exit(f'{path}: not a VOC2 book')
    off_word, off_str = struct.unpack_from('<6I', d, 16)[4:6]
    out = []
    for i in range(words):
        p = struct.unpack_from('<I', d, off_word + 4 * i)[0]
        fields = d[off_str + p:].split(b'\0', 7)
        out.append((fields[0].decode('utf-8'), fields[5].decode('utf-8')))
    return out


def key_of(en):
    return re.sub(r'[^a-z0-9]', '_', en.lower())


def synth(engine, voice, rate, text, raw):
    if engine == 'say':
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False, encoding='utf-8') as f:
            f.write(text)
        try:
            cmd = ['say', '-v', voice, '-o', raw, '-f', f.name]
            if rate:
                cmd[1:1] = ['-r', str(rate)]
            subprocess.run(cmd, check=True, capture_output=True)
        finally:
            os.unlink(f.name)
    else:
        cmd = ['edge-tts', '--voice', voice, f'--text={text}', '--write-media', raw]
        if rate:
            cmd.append(f'--rate={rate:+d}%')
        subprocess.run(cmd, check=True, capture_output=True)


def make(job, engine, voice, rate, bitrate):
    text, dest = job
    if dest.exists() and dest.stat().st_size > 0:
        return 'skip'
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'raw.aiff' if engine == 'say' else 'raw.mp3')
        synth(engine, voice, rate, text, raw)
        part = os.path.join(tmp, 'out.mp3')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', raw, '-af', TRIM, '-ar', '16000', '-ac', '1',
                        '-codec:a', 'libmp3lame', '-b:a', bitrate, '-map_metadata', '-1', '-id3v2_version', '0',
                        '-write_xing', '0', part], check=True, capture_output=True)
        shutil.move(part, dest)
    return 'made'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--engine', choices=['say', 'edge'], default='say')
    ap.add_argument('--voice', help='say: Samantha / Daniel …；edge: en-US-AriaNeural / en-GB-SoniaNeural …')
    ap.add_argument('--rate', type=int, default=0,
                    help='say: 每分钟词数（如 150）/ words per minute; edge: 百分比（如 -10）/ percent')
    ap.add_argument('--bitrate', default='32k')
    ap.add_argument('--book', type=Path, default=BOOK)
    ap.add_argument('--out', type=Path, default=OUT)
    ap.add_argument('--words-only', action='store_true')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    voice = args.voice or DEFAULT_VOICE[args.engine]
    need = ['ffmpeg', 'say' if args.engine == 'say' else 'edge-tts']
    missing = [t for t in need if not shutil.which(t)]
    if missing:
        sys.exit('missing: ' + ', '.join(missing))

    words = read_book(args.book)
    seen = {}
    for en, _ in words:
        k = key_of(en)
        if k in seen and seen[k] != en:
            sys.exit(f'file name clash: {seen[k]!r} and {en!r} both map to {k!r}')
        seen[k] = en
    (args.out / 'w').mkdir(parents=True, exist_ok=True)
    (args.out / 's').mkdir(parents=True, exist_ok=True)
    jobs = [(en, args.out / 'w' / f'{key_of(en)}.mp3') for en, _ in words]
    if not args.words_only:
        jobs += [(ex, args.out / 's' / f'{key_of(en)}.mp3') for en, ex in words if ex.strip()]

    done = made = 0
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(make, j, args.engine, voice, args.rate, args.bitrate): j for j in jobs}
        for fut in concurrent.futures.as_completed(futures):
            done += 1
            try:
                made += fut.result() == 'made'
            except subprocess.CalledProcessError as e:
                failed.append((futures[fut][0], (e.stderr or b'').decode(errors='replace').strip()))
            if done % 100 == 0 or done == len(jobs):
                print(f'{done}/{len(jobs)}  new {made}  failed {len(failed)}', flush=True)
    for text, err in failed[:20]:
        print(f'FAILED {text!r}: {err}', file=sys.stderr)
    size = sum(p.stat().st_size for p in args.out.rglob('*.mp3'))
    print(f'{args.out}: {size / 1e6:.1f} MB. Copy it to the SD card as /vocab_audio.')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
