#!/usr/bin/env bash
# 耗电日志主机测试。/ Host test for the battery drain log.
set -euo pipefail
cd "$(dirname "$0")/.."
out=build/book-tests/power-log
mkdir -p "$out"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    "-DPOWER_LOG_PATH=\"$out/power_log.csv\"" "-DPOWER_LOG_OLD_PATH=\"$out/power_log.old.csv\"" \
    tools/power_log_host_test.c main/power_log.c -o "$out/test"
"$out/test"
