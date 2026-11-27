/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "auto_lock.h"
int main(void) {
    const int64_t input = INT64_C(1000000);
    for (unsigned minutes = 1; minutes <= 10; minutes += minutes == 1 ? 4 : 5) {
        // 中文：Metalio47 最短一档存值仍是 1，时长 90 秒；其余档按分钟。
        // English: On Metalio47 the shortest option is still stored as 1 but lasts 90 s; the others are minutes.
        const int64_t length = minutes == 1 ? INT64_C(90000) : (int64_t)minutes * 60000;
        assert(auto_lock_ms(minutes) == length);
        const int64_t deadline = input + length;
        assert(!auto_lock_due(deadline - 1, input, minutes));
        assert(auto_lock_due(deadline, input, minutes));
        assert(auto_lock_due(deadline + 1, input, minutes));
        assert(!auto_lock_due(deadline, deadline, minutes)); // User input resets the interval.
        assert(!auto_lock_due(input - 1, input, minutes));
    }
    assert(!auto_lock_due(input + 60000, input, 1));  // 一分钟时还不锁 / not yet locked at one minute
    assert(!auto_lock_due(INT64_C(1000000000000), input, 0));
    assert(!auto_lock_due(INT64_C(1000000000000), input, 2));
    assert(!auto_lock_due(INT64_C(1000000000000), input, 255));
    puts("auto lock: off, supported intervals, boundary and input reset passed");
}
