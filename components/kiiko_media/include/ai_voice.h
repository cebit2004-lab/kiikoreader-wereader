/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：AI 语音对话（通义千问 Omni 实时接口，WebSocket）。免按住：开始后直接说话，服务端判断说完即回答，
 * 可连续对话；一段时间没人说话自动结束。板子没有回声消除，AI 说话时不上传麦克风，靠按键打断。
 * 开始对话会先停掉音乐，WiFi 与 I2S 只给 AI 用。
 *
 * English: AI voice chat (Qwen Omni realtime over WebSocket). Hands-free: after starting, just talk;
 * the server detects the end of speech and replies, and the chat continues until a quiet spell ends
 * it. The board has no echo cancellation, so the microphone is not uploaded while the AI speaks;
 * interrupt with the key. Starting a chat stops music so WiFi and I2S belong to the AI.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_IDLE,        // 未对话 / not chatting
    AI_CONNECTING,  // 联网、连接中 / joining WiFi and connecting
    AI_LISTENING,   // 等你说话 / waiting for speech
    AI_HEARING,     // 正在听 / speech in progress
    AI_THINKING,    // 说完了，等回答 / speech ended, awaiting the reply
    AI_SPEAKING,    // 正在回答 / replying
} ai_state_t;

#define AI_USER_MAX 384
#define AI_REPLY_MAX 2048

typedef struct {
    ai_state_t state;
    char user[AI_USER_MAX];     // 最近一次识别出的你说的话 / your latest transcribed words
    char reply[AI_REPLY_MAX];   // AI 的回答（流式累积）/ the AI's reply, accumulated as it streams
    char error[96];             // 最近的错误，空表示无 / last error, empty if none
    int mic_peak;               // 最近一段上传的峰值（放大后）/ recent upload peak (after gain)
    unsigned revision;          // 任何变化都递增 / bumps on any change
} ai_status_t;

esp_err_t ai_init(void);
/// 开始对话（已在对话中则无动作）。/ Start a chat (no-op while one runs).
void ai_start(void);
/// 结束对话。/ End the chat.
void ai_stop(void);
/// 未对话则开始；AI 正在说则打断；否则结束。/ Start when idle; interrupt while the AI speaks; otherwise end.
void ai_toggle(void);
void ai_get_status(ai_status_t *out);
/// 对话进行中（占用网络与音频）。/ A chat is running (holding the network and audio).
bool ai_active(void);

#ifdef __cplusplus
}
#endif
