/* 中文：调度测试替身。/ English: Scheduler test shim. */
#pragma once
#include "common.h"

bool app_sleep_media_ready(void);
bool app_sleep_media_failed(void);
void app_sleep_discard_media_io(void);
