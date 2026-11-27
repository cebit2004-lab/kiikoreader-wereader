#!/usr/bin/env bash
# 背单词进度文件主机测试。/ Host test for the vocab progress file.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Itools/vocab_progress_stubs -Imain/apps \
    tools/vocab_progress_host_test.c -o build/book-tests/vocab-progress
build/book-tests/vocab-progress main/assets/vocab.bin
