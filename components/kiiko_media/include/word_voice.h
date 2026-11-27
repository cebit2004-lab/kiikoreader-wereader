/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：短音频点读（背单词发音）。播放卡上的小 MP3（单词、例句，tools/gen_vocab_audio.py 生成，
 * 16 kHz 单声道），不联网。新的一段会打断正在读的；音乐或 AI 占着喇叭时不读。读完立即放开 I2S。
 *
 * English: Short clip playback for vocabulary pronunciation. Plays small MP3s from the card (words and
 * examples, made by tools/gen_vocab_audio.py at 16 kHz mono), with no network. A new clip cuts off the
 * one playing; nothing plays while music or the AI holds the speaker. I2S is let go right after a clip.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 读入 path 并开始播放，打断正在读的。文件不存在、太大或喇叭被音乐/AI 占着返回 false。
/// 在调用方任务里读卡，栈要在内部 RAM。
/// Load path and start playing it, cutting off any clip in progress. Returns false when the file is
/// missing or too large, or music/the AI holds the speaker. Reads the card on the caller's task, which
/// must have an internal-RAM stack.
bool word_voice_play(const char *path);
/// 停止并丢掉等着的。/ Stop and drop anything queued.
void word_voice_stop(void);
/// 正在读或有一段等着读。/ A clip is playing or queued.
bool word_voice_busy(void);
/// 提前把音频芯片切到可用（开机后首次约 2 秒），不等待。/ Wake the audio chip early (about 2 s on first use), without waiting.
void word_voice_prepare(void);

#ifdef __cplusplus
}
#endif
