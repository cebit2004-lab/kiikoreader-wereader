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
    char error[192];            // 最近的错误（含原因），空表示无 / last error with its cause, empty if none
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
/// 已关闭的 AI 连接还有任务在收尾（它的套接字可能还开着）。此时停 WiFi，它手里的接收缓冲会在驱动
/// 卸载后才归还，所以要等它退出。
/// A closed AI connection still has a task shutting down (its socket may still be open). Stopping WiFi
/// now would let it return receive buffers after the driver is gone, so wait for it to exit.
bool ai_net_busy(void);
/// AI 什么都不占了：没有对话、没有预热（含进行中的）、收尾（停 WiFi、写日志）已做完、I2S 已关、旧连接已退出
/// 或已放弃等待。ai_active() 在收尾开始时就变假（WiFi 收回要看它），要等全部收完的用这个。
/// The AI holds nothing: no chat, no warm-up (including one in progress), teardown (WiFi stop, log
/// write) finished, I2S closed, and the old connection gone or given up on. ai_active() turns false as
/// teardown begins (the WiFi release depends on that); use this to wait for everything to be released.
bool ai_fully_released(void);
/// AI 页进出时调用。进页：后台先开音频再连 WiFi（不连 AI 服务），点开始时直接连服务；对话结束后页面还开着
/// 就留着这两样，90 秒没有新对话才收回。离页：立即收回（对话由页面另行 ai_stop）。
/// Call on entering and leaving the AI page. Entering: audio, then WiFi, come up in the background
/// (without the AI service), so start goes straight to the service; after a chat both stay while the
/// page is open, released after 90 s without a new chat. Leaving: released at once (the page stops any
/// chat itself with ai_stop).
void ai_page_open(bool open);
/// 预热持有着音频和 WiFi（不在对话中）：这期间不要进入休眠。
/// The warm-up holds audio and WiFi (no chat running): do not go idle meanwhile.
bool ai_warm_held(void);
/// AI 说话时界面想刷屏就调用：返回 true 表示语音正处在停顿（或尚未起播），现在刷屏不会出破音，
/// 播放会暂停等刷完；返回 false 表示已登记，等下一个停顿。刷完后（下一次 tick）调用 ai_redraw_done。
/// Call while the AI speaks and the page wants to redraw: true means speech is at a pause (or not yet
/// playing), so a refresh now cannot crackle and playback waits for it; false means the request is
/// recorded for the next pause. Call ai_redraw_done once the refresh is complete (next tick).
bool ai_redraw_slot(void);
void ai_redraw_done(void);
/// 回答正在生成或播放：此时不走 ai_redraw_slot 的刷屏（如状态栏电量）应推迟。
/// A reply is being generated or played: refreshes that do not go through ai_redraw_slot (such as the
/// status-bar battery level) should wait.
bool ai_audio_busy(void);

#ifdef __cplusplus
}
#endif
