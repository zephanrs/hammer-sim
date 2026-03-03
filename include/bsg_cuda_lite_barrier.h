#pragma once

#include "../runtime.hpp"

inline void bsg_barrier_tile_group_init() {
  hb_native_sim::barrier_init();
}

inline void bsg_barrier_tile_group_sync() {
  hb_native_sim::barrier_sync();
}

inline void bsg_cuda_print_stat_kernel_start() {
  hb_native_sim::kernel_start();
}

inline void bsg_cuda_print_stat_kernel_end() {
  hb_native_sim::kernel_end();
}
