/* 中文：主循环测试用的 AI 语音接口替身，只声明主循环用到的函数。/ English: AI voice API stand-in for the loop test; declares only what the loop uses. */
#pragma once
#include <stdbool.h>
bool ai_active(void);
bool ai_audio_busy(void);
bool ai_net_busy(void);
bool ai_warm_held(void);
void ai_start(void);
void ai_toggle(void);
