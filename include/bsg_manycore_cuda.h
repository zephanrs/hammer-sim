#pragma once

#include "bsg_manycore.h"
#include "bsg_manycore_errno.h"
#include "../runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

struct hb_mc_dma_htod_t {
  eva_t d_addr;
  const void* h_addr;
  std::size_t size;
};

struct hb_mc_dma_dtoh_t {
  eva_t d_addr;
  void* h_addr;
  std::size_t size;
};

struct hb_mc_device_t {
  hb_native_sim::device_state* state = nullptr;
  hb_native_sim::manycore_t* mc = nullptr;
  hb_mc_pod_id_t default_pod = 0;
  int pod_count = 1;
};

#define BSG_CUDA_CALL(expr)                                                     \
  do {                                                                          \
    const int hb_native_rc = (expr);                                            \
    if (hb_native_rc != HB_MC_SUCCESS) {                                        \
      return hb_native_rc;                                                      \
    }                                                                           \
  } while (0)

#define hb_mc_device_foreach_pod_id(device_ptr, pod_var)                        \
  for (hb_mc_pod_id_t pod_var = 0; pod_var < (device_ptr)->pod_count; ++pod_var)

inline int hb_mc_device_init(hb_mc_device_t* device, const char*, int) {
  device->state = new hb_native_sim::device_state();
  device->mc = new hb_native_sim::manycore_t();
  return HB_MC_SUCCESS;
}

inline int hb_mc_device_set_default_pod(hb_mc_device_t* device, hb_mc_pod_id_t pod) {
  device->default_pod = pod;
  return HB_MC_SUCCESS;
}

inline int hb_mc_device_program_init(hb_mc_device_t*, const char*, const char*, int) {
  return HB_MC_SUCCESS;
}

inline int hb_mc_device_malloc(hb_mc_device_t* device, std::size_t size, eva_t* out) {
  constexpr std::size_t kAlign = 64;
  std::size_t start = device->state->next_alloc;
  start = (start + (kAlign - 1)) & ~(kAlign - 1);
  const std::size_t end = start + size;
  if (end > device->state->memory.size()) {
    device->state->memory.resize(end);
  }
  *out = static_cast<eva_t>(start);
  device->state->next_alloc = static_cast<eva_t>(end);
  return HB_MC_SUCCESS;
}

inline int hb_mc_device_transfer_data_to_device(hb_mc_device_t* device,
                                                const hb_mc_dma_htod_t* jobs,
                                                std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    std::memcpy(device->state->memory.data() + jobs[i].d_addr, jobs[i].h_addr, jobs[i].size);
  }
  return HB_MC_SUCCESS;
}

inline int hb_mc_device_transfer_data_to_host(hb_mc_device_t* device,
                                              const hb_mc_dma_dtoh_t* jobs,
                                              std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    std::memcpy(jobs[i].h_addr, device->state->memory.data() + jobs[i].d_addr, jobs[i].size);
  }
  return HB_MC_SUCCESS;
}

inline int hb_mc_kernel_enqueue(hb_mc_device_t* device,
                                hb_mc_dimension_t,
                                hb_mc_dimension_t tile_group_dim,
                                const char* kernel_name,
                                std::uint32_t argc,
                                const std::uint32_t* argv) {
  hb_native_sim::kernel_launch launch;
  launch.name = kernel_name;
  launch.tile_group_x = tile_group_dim.x;
  launch.tile_group_y = tile_group_dim.y;
  launch.argv.assign(argv, argv + argc);
  device->state->launches.push_back(std::move(launch));
  return HB_MC_SUCCESS;
}

inline int hb_mc_device_pods_kernels_execute(hb_mc_device_t* device) {
  for (const auto& launch : device->state->launches) {
    hb_native_sim::run_kernel(*device->state, launch);
  }
  device->state->launches.clear();
  return HB_MC_SUCCESS;
}

inline int hb_mc_device_finish(hb_mc_device_t* device) {
  delete device->mc;
  device->mc = nullptr;
  delete device->state;
  device->state = nullptr;
  return HB_MC_SUCCESS;
}
