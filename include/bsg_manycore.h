#pragma once

#include "../runtime.hpp"

#include <atomic>
#include <cstdint>

using eva_t = hb_native_sim::eva_t;

struct hb_mc_dimension_t {
  std::uint32_t x;
  std::uint32_t y;
};

using hb_mc_pod_id_t = std::uint32_t;

#define bsg_tiles_X TILE_GROUP_DIM_X
#define bsg_tiles_Y TILE_GROUP_DIM_Y
#define HB_MC_DEVICE_ID 0
#define bsg_unroll(x)
#define __bsg_x (hb_native_sim::current_x())
#define __bsg_y (hb_native_sim::current_y())

inline void* bsg_remote_ptr(int x, int y, const void* ptr) {
  return hb_native_sim::remote_ptr(x, y, ptr);
}

using hb_native_sim::bsg_lr;
using hb_native_sim::bsg_lr_aq;

inline void bsg_fence() {
  hb_native_sim::fence();
}

inline void hb_mc_manycore_trace_enable(hb_native_sim::manycore_t*) {}
inline void hb_mc_manycore_trace_disable(hb_native_sim::manycore_t*) {}
