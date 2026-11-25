/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：PSRAM 流缓冲的创建与删除。ESP-IDF 6.1 的 vStreamBufferDeleteWithCaps 误用 vSemaphoreDelete
 * （即 vQueueDelete）删除流缓冲：它按队列结构去读流缓冲结构体之外的一个字节，判断要不要释放；那个字节
 * 碰巧是 0 时先释放一次，随后自己再释放一次，堆断言“block already marked as free”崩溃。这里自己分配
 * 结构体和存储区，用 vStreamBufferDelete 删除后再各释放一次。
 *
 * English: Create and delete PSRAM stream buffers. ESP-IDF 6.1's vStreamBufferDeleteWithCaps deletes
 * the stream buffer with vSemaphoreDelete (i.e. vQueueDelete), which reads a byte beyond the stream
 * buffer struct as if it were a queue's "statically allocated" flag; when that byte happens to be 0 it
 * frees the struct, then frees it again itself, and the heap asserts "block already marked as free".
 * Here the struct and storage are allocated by hand, deleted with vStreamBufferDelete, then each freed
 * once.
 */
#pragma once

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

static inline StreamBufferHandle_t psram_stream_create(size_t size, size_t trigger) {
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *storage = heap_caps_malloc(size, caps);
    StaticStreamBuffer_t *meta = heap_caps_malloc(sizeof(*meta), caps);
    StreamBufferHandle_t handle = storage && meta ? xStreamBufferCreateStatic(size, trigger, storage, meta) : NULL;
    if (!handle) {
        heap_caps_free(meta);
        heap_caps_free(storage);
    }
    return handle;
}

static inline void psram_stream_delete(StreamBufferHandle_t handle) {
    if (!handle) return;
    uint8_t *storage = NULL;
    StaticStreamBuffer_t *meta = NULL;
    // 取不到就宁可漏掉，也不乱释放。/ If the buffers cannot be found, leak rather than free blindly.
    if (xStreamBufferGetStaticBuffers(handle, &storage, &meta) != pdTRUE) return;
    vStreamBufferDelete(handle);
    heap_caps_free(meta);
    heap_caps_free(storage);
}
