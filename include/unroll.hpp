#pragma once

#include <stdint.h>

#include "../runtime.hpp"

template<typename T, int N, int S = 1>
static inline __attribute__((always_inline))
void unrolled_load(T* __restrict dst,
                   const T* __restrict src)
{
    T buf[N];

    bsg_unroll(24)
    for (int i = 0; i < N; i++) {
        register T r = hb_native_sim::load(&src[i * S]);
        buf[i] = r;
    }

    asm volatile("" ::: "memory");

    bsg_unroll(24)
    for (int i = 0; i < N; i++) {
        hb_native_sim::store(&dst[i * S], buf[i]);
    }
}
