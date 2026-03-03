#include "runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace hb_native_sim {
namespace {

int (*g_program_main)(int, char**) = nullptr;
std::unordered_map<std::string, kernel_descriptor> g_kernels;
thread_local core_context* g_current_core = nullptr;

constexpr std::size_t k_lr_word_size = sizeof(int);

const std::uint8_t* as_bytes(const volatile void* addr) {
  return static_cast<const std::uint8_t*>(const_cast<void*>(addr));
}

std::size_t ceil_div(std::size_t value, std::size_t divisor) {
  return (value + divisor - 1) / divisor;
}

reservation_word_t& reservation_word_for_offset(simulation_state& simulation,
                                                std::size_t tile_index,
                                                std::size_t local_offset) {
  const std::size_t word_index = (tile_index * simulation.words_per_tile) + (local_offset / k_lr_word_size);
  return simulation.scratchpad_words[word_index];
}

struct scratchpad_span_t {
  bool in_scratchpad = false;
  std::size_t first_tile = 0;
  std::size_t last_tile = 0;
};

scratchpad_span_t scratchpad_span_for(simulation_state& simulation,
                                      const volatile void* addr,
                                      std::size_t size) {
  scratchpad_span_t span;
  if (size == 0 || simulation.scratchpad_block == nullptr || simulation.scratchpad_size == 0) {
    return span;
  }

  const auto* block = static_cast<const std::uint8_t*>(simulation.scratchpad_block);
  const std::size_t tile_count = simulation.statuses.size();
  const std::size_t block_size = tile_count * simulation.scratchpad_size;
  const auto* ptr = as_bytes(addr);
  if (ptr < block || ptr >= (block + block_size)) {
    return span;
  }

  const std::size_t start_offset = static_cast<std::size_t>(ptr - block);
  const std::size_t end_offset = start_offset + size - 1;
  if (end_offset >= block_size) {
    throw std::runtime_error("scratchpad access out of range");
  }

  span.in_scratchpad = true;
  span.first_tile = start_offset / simulation.scratchpad_size;
  span.last_tile = end_offset / simulation.scratchpad_size;
  return span;
}

template <typename Fn>
auto with_scratchpad_locks(simulation_state& simulation,
                           const volatile void* addr,
                           std::size_t size,
                           Fn&& fn) -> decltype(fn()) {
  const scratchpad_span_t span = scratchpad_span_for(simulation, addr, size);
  if (!span.in_scratchpad) {
    return fn();
  }

  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(span.last_tile - span.first_tile + 1);
  for (std::size_t tile = span.first_tile; tile <= span.last_tile; ++tile) {
    locks.emplace_back(simulation.scratchpad_mutexes[tile]);
  }
  return fn();
}

void copy_from_volatile(const volatile void* src, void* dst, std::size_t size) {
  auto* out = static_cast<std::uint8_t*>(dst);
  auto* bytes = static_cast<const volatile std::uint8_t*>(src);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = bytes[i];
  }
}

void copy_to_volatile(volatile void* dst, const void* src, std::size_t size) {
  auto* out = static_cast<volatile std::uint8_t*>(dst);
  auto* bytes = static_cast<const std::uint8_t*>(src);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = bytes[i];
  }
}

int load_int_prefix(const volatile void* addr) {
  int value = 0;
  copy_from_volatile(addr, &value, sizeof(value));
  return value;
}

reservation_word_t& local_reservation_word(core_context& context, const volatile void* addr) {
  const auto* local_base = static_cast<const std::uint8_t*>(context.scratchpad_base);
  const auto* ptr = as_bytes(addr);
  const std::ptrdiff_t offset = ptr - local_base;

  if (offset < 0 || static_cast<std::size_t>(offset) + k_lr_word_size > context.scratchpad_size) {
    throw std::runtime_error("bsg_lr source outside local scratchpad");
  }

  return reservation_word_for_offset(*context.simulation,
                                     context.core_index,
                                     static_cast<std::size_t>(offset));
}

int load_reserved_word(const volatile void* addr, reservation_word_t& word, std::uint64_t* epoch_out = nullptr) {
  core_context& context = current_core_context();
  int value = 0;
  std::uint64_t epoch = 0;
  with_scratchpad_locks(*context.simulation, addr, sizeof(value), [&] {
    epoch = word.epoch.load(std::memory_order_acquire);
    value = load_int_prefix(addr);
  });
  if (epoch_out != nullptr) {
    *epoch_out = epoch;
  }
  return value;
}

void notify_store_words(simulation_state& simulation, const volatile void* addr, std::size_t size) {
  if (size == 0 || simulation.words_per_tile == 0) {
    return;
  }

  const scratchpad_span_t span = scratchpad_span_for(simulation, addr, size);
  if (!span.in_scratchpad) {
    return;
  }

  const auto* block = static_cast<const std::uint8_t*>(simulation.scratchpad_block);
  const std::size_t start_offset = static_cast<std::size_t>(as_bytes(addr) - block);
  const std::size_t end_offset = start_offset + size - 1;
  for (std::size_t tile = span.first_tile; tile <= span.last_tile; ++tile) {
    const std::size_t tile_base = tile * simulation.scratchpad_size;
    const std::size_t local_start = (tile == span.first_tile) ? (start_offset - tile_base) : 0;
    const std::size_t local_end = (tile == span.last_tile) ? (end_offset - tile_base) : (simulation.scratchpad_size - 1);
    const std::size_t start_word = local_start / k_lr_word_size;
    const std::size_t end_word = local_end / k_lr_word_size;

    for (std::size_t word = start_word; word <= end_word; ++word) {
      auto& epoch = reservation_word_for_offset(simulation, tile, word * k_lr_word_size).epoch;
      epoch.fetch_add(1, std::memory_order_acq_rel);
      epoch.notify_all();
    }
  }
}

void set_core_status(core_context& context, core_status status) {
  context.simulation->statuses[context.core_index].store(status, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->wait_targets[context.core_index].store(0, std::memory_order_release);
}

void set_memory_wait_state(core_context& context,
                           reservation_word_t* waited_word,
                           std::uint64_t target_epoch) {
  context.simulation->statuses[context.core_index].store(core_status::waiting_memory, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(waited_word, std::memory_order_release);
  context.simulation->wait_targets[context.core_index].store(target_epoch, std::memory_order_release);
}

void set_barrier_state(core_context& context, std::uint64_t target_generation) {
  context.simulation->statuses[context.core_index].store(core_status::waiting_barrier, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->wait_targets[context.core_index].store(target_generation, std::memory_order_release);
}

bool core_can_run(const simulation_state& simulation, const barrier_t& barrier, std::size_t core_index) {
  const core_status status = simulation.statuses[core_index].load(std::memory_order_acquire);
  switch (status) {
    case core_status::starting:
    case core_status::running:
      return true;
    case core_status::terminated:
    case core_status::failed:
      return false;
    case core_status::waiting_memory: {
      reservation_word_t* waited_word = simulation.waited_words[core_index].load(std::memory_order_acquire);
      const std::uint64_t target = simulation.wait_targets[core_index].load(std::memory_order_acquire);
      return waited_word != nullptr && waited_word->epoch.load(std::memory_order_acquire) != target;
    }
    case core_status::waiting_barrier: {
      const std::uint64_t target = simulation.wait_targets[core_index].load(std::memory_order_acquire);
      return barrier.generation.load(std::memory_order_acquire) != target;
    }
  }
  return false;
}

void request_abort(simulation_state& simulation, barrier_t& barrier) {
  simulation.abort_requested.store(true, std::memory_order_release);
  barrier.generation.fetch_add(1, std::memory_order_acq_rel);
  barrier.generation.notify_all();

  for (auto& word : simulation.scratchpad_words) {
    word.epoch.fetch_add(1, std::memory_order_acq_rel);
    word.epoch.notify_all();
  }
}

void throw_if_abort_requested() {
  core_context& context = current_core_context();
  if (context.simulation != nullptr &&
      context.simulation->abort_requested.load(std::memory_order_acquire)) {
    throw std::runtime_error("deadlock detected");
  }
}

template <typename Op>
int atomic_fetch_op(int* addr, int value, Op op) {
  std::atomic_ref<int> atomic_ref(*addr);
  int current = atomic_ref.load(std::memory_order_relaxed);
  while (!atomic_ref.compare_exchange_weak(current,
                                           op(current, value),
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
  }
  return current;
}

}  // namespace

void barrier_t::init(std::size_t count) {
  participants = count;
  arrived.store(0, std::memory_order_relaxed);
  generation.store(0, std::memory_order_relaxed);
}

void register_program_main(int (*fn)(int, char**)) {
  g_program_main = fn;
}

int (*program_main())(int, char**) {
  return g_program_main;
}

void register_kernel(kernel_descriptor descriptor) {
  g_kernels.emplace(descriptor.name, std::move(descriptor));
}

const kernel_descriptor& lookup_kernel(const std::string& name) {
  const auto it = g_kernels.find(name);
  if (it == g_kernels.end()) {
    throw std::runtime_error("kernel not registered: " + name);
  }
  return it->second;
}

void set_current_core_context(core_context* context) {
  g_current_core = context;
}

core_context& current_core_context() {
  if (g_current_core == nullptr) {
    throw std::runtime_error("no active core context");
  }
  return *g_current_core;
}

void* current_scratchpad_base() {
  return current_core_context().scratchpad_base;
}

void* remote_ptr(int x, int y, const void* local_ptr) {
  core_context& context = current_core_context();
  const auto* local_base = static_cast<const std::uint8_t*>(context.scratchpad_base);
  const auto* local = static_cast<const std::uint8_t*>(local_ptr);
  const std::ptrdiff_t offset = local - local_base;

  if (offset < 0 || static_cast<std::size_t>(offset) >= context.scratchpad_size) {
    throw std::runtime_error("remote_ptr source outside scratchpad");
  }

  if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= context.tile_group_x ||
      static_cast<std::size_t>(y) >= context.tile_group_y) {
    return static_cast<std::uint8_t*>(context.scratchpad_base) + offset;
  }

  const std::size_t index = (static_cast<std::size_t>(x) * context.tile_group_y) + static_cast<std::size_t>(y);
  auto* base = static_cast<std::uint8_t*>(context.scratchpad_block);
  return base + (index * context.scratchpad_size) + offset;
}

void* eva_to_ptr(eva_t addr) {
  core_context& context = current_core_context();
  if (addr >= context.device->memory.size()) {
    throw std::runtime_error("eva out of range");
  }
  return context.device->memory.data() + addr;
}

int current_x() {
  return current_core_context().x;
}

int current_y() {
  return current_core_context().y;
}

int bsg_lr(const volatile void* addr) {
  core_context& context = current_core_context();
  reservation_word_t& word = local_reservation_word(context, addr);
  context.reservation.addr = addr;
  context.reservation.word = &word;
  return load_reserved_word(addr, word, &context.reservation.epoch);
}

int bsg_lr_aq(const volatile void* addr) {
  core_context& context = current_core_context();
  reservation_word_t& word =
      (context.reservation.addr == addr && context.reservation.word != nullptr)
          ? *context.reservation.word
          : local_reservation_word(context, addr);
  const std::uint64_t target =
      (context.reservation.addr == addr && context.reservation.word == &word)
          ? context.reservation.epoch
          : word.epoch.load(std::memory_order_acquire);
  set_memory_wait_state(context, &word, target);

  std::uint64_t observed_epoch = word.epoch.load(std::memory_order_acquire);
  while (word.epoch.load(std::memory_order_acquire) == target) {
    word.epoch.wait(observed_epoch, std::memory_order_acquire);
    throw_if_abort_requested();
    observed_epoch = word.epoch.load(std::memory_order_acquire);
  }

  std::this_thread::yield();
  context.reservation = {};
  set_core_status(context, core_status::running);
  return load_reserved_word(addr, word);
}

void load_bytes(const volatile void* addr, void* out, std::size_t size) {
  core_context& context = current_core_context();
  with_scratchpad_locks(*context.simulation, addr, size, [&] {
    copy_from_volatile(addr, out, size);
  });
}

void store_bytes(void* addr, const void* value, std::size_t size) {
  store_bytes(static_cast<volatile void*>(addr), value, size);
}

void store_bytes(volatile void* addr, const void* value, std::size_t size) {
  core_context& context = current_core_context();
  with_scratchpad_locks(*context.simulation, addr, size, [&] {
    copy_to_volatile(addr, value, size);
    notify_store_words(*context.simulation, addr, size);
  });
}

void store_int_word(int* addr, int value) {
  core_context& context = current_core_context();
  const scratchpad_span_t span = scratchpad_span_for(*context.simulation, addr, sizeof(int));
  if (span.in_scratchpad) {
    store_bytes(addr, &value, sizeof(value));
    return;
  }

  std::atomic_ref<int> atomic_ref(*addr);
  atomic_ref.store(value, std::memory_order_seq_cst);
}

void store_int_word(volatile int* addr, int value) {
  store_int_word(const_cast<int*>(addr), value);
}

void barrier_init() {
}

void barrier_sync() {
  core_context& context = current_core_context();
  barrier_t& barrier = *context.barrier;
  const std::uint64_t target_generation = barrier.generation.load(std::memory_order_acquire);
  const std::size_t prior = barrier.arrived.fetch_add(1, std::memory_order_acq_rel);
  if (prior + 1 == barrier.participants) {
    barrier.arrived.store(0, std::memory_order_release);
    barrier.generation.fetch_add(1, std::memory_order_acq_rel);
    barrier.generation.notify_all();
    return;
  }

  set_barrier_state(context, target_generation);
  while (barrier.generation.load(std::memory_order_acquire) == target_generation) {
    barrier.generation.wait(target_generation, std::memory_order_acquire);
    throw_if_abort_requested();
  }
  set_core_status(context, core_status::running);
}

void kernel_start() {
}

void kernel_end() {
}

void fence() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

int run_kernel(device_state& device, const kernel_launch& launch) {
  const kernel_descriptor& kernel = lookup_kernel(launch.name);
  const std::size_t tile_count = static_cast<std::size_t>(launch.tile_group_x) * launch.tile_group_y;
  if (tile_count == 0) {
    throw std::runtime_error("tile group must be non-zero");
  }

  void* scratchpad_block = ::operator new[](tile_count * kernel.scratchpad_size,
                                            std::align_val_t(kernel.scratchpad_align));
  kernel.construct(scratchpad_block, tile_count);

  barrier_t barrier;
  barrier.init(tile_count);

  const std::size_t words_per_tile = ceil_div(kernel.scratchpad_size, k_lr_word_size);
  simulation_state simulation(tile_count, tile_count * words_per_tile);
  simulation.scratchpad_block = scratchpad_block;
  simulation.scratchpad_size = kernel.scratchpad_size;
  simulation.words_per_tile = words_per_tile;
  for (auto& status : simulation.statuses) {
    status.store(core_status::starting, std::memory_order_relaxed);
  }
  for (auto& waited_word : simulation.waited_words) {
    waited_word.store(nullptr, std::memory_order_relaxed);
  }
  for (auto& wait_target : simulation.wait_targets) {
    wait_target.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> threads;
  threads.reserve(tile_count);
  std::thread watchdog;
  std::exception_ptr thread_error = nullptr;
  std::mutex error_mutex;

  watchdog = std::thread([&] {
    std::size_t quiescent_polls = 0;
    while (!simulation.abort_requested.load(std::memory_order_acquire)) {
      std::size_t terminated_or_failed = 0;
      bool any_runnable = false;

      for (std::size_t i = 0; i < tile_count; ++i) {
        const core_status status = simulation.statuses[i].load(std::memory_order_acquire);
        if (status == core_status::terminated || status == core_status::failed) {
          ++terminated_or_failed;
        }
        if (core_can_run(simulation, barrier, i)) {
          any_runnable = true;
        }
      }

      if (terminated_or_failed == tile_count) {
        return;
      }

      if (!any_runnable) {
        ++quiescent_polls;
        if (quiescent_polls < 64) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        simulation.deadlock_detected.store(true, std::memory_order_release);
        request_abort(simulation, barrier);
        return;
      }

      quiescent_polls = 0;
      std::this_thread::yield();
    }
  });

  for (std::uint32_t x = 0; x < launch.tile_group_x; ++x) {
    for (std::uint32_t y = 0; y < launch.tile_group_y; ++y) {
      threads.emplace_back([&, x, y] {
        core_context context;
        context.core_index = (static_cast<std::size_t>(x) * launch.tile_group_y) + y;
        context.x = static_cast<int>(x);
        context.y = static_cast<int>(y);
        context.scratchpad_block = scratchpad_block;
        context.scratchpad_size = kernel.scratchpad_size;
        context.tile_group_x = launch.tile_group_x;
        context.tile_group_y = launch.tile_group_y;
        context.barrier = &barrier;
        context.device = &device;
        context.simulation = &simulation;
        context.scratchpad_base = static_cast<std::uint8_t*>(scratchpad_block) +
                                  (((static_cast<std::size_t>(x) * launch.tile_group_y) + y) * kernel.scratchpad_size);
        set_current_core_context(&context);
        try {
          set_core_status(context, core_status::running);
          kernel.invoke(launch.argv.data());
          set_core_status(context, core_status::terminated);
        } catch (...) {
          set_core_status(context, core_status::failed);
          request_abort(simulation, barrier);
          std::lock_guard<std::mutex> lock(error_mutex);
          if (thread_error == nullptr) {
            thread_error = std::current_exception();
          }
        }
        set_current_core_context(nullptr);
      });
    }
  }

  for (auto& thread : threads) {
    thread.join();
  }

  simulation.abort_requested.store(true, std::memory_order_release);
  if (watchdog.joinable()) {
    watchdog.join();
  }

  kernel.destroy(scratchpad_block, tile_count);
  ::operator delete[](scratchpad_block, std::align_val_t(kernel.scratchpad_align));

  if (simulation.deadlock_detected.load(std::memory_order_acquire) && thread_error == nullptr) {
    throw std::runtime_error("deadlock detected");
  }

  if (thread_error != nullptr) {
    std::rethrow_exception(thread_error);
  }

  return 0;
}

int amoadd_w(int* addr, int value) {
  std::atomic_ref<int> atomic_ref(*addr);
  return atomic_ref.fetch_add(value, std::memory_order_acq_rel);
}

int amoswap_w(int* addr, int value) {
  std::atomic_ref<int> atomic_ref(*addr);
  return atomic_ref.exchange(value, std::memory_order_acq_rel);
}

int amoor_w(int* addr, int value) {
  return atomic_fetch_op(addr, value, [](int current, int next) { return current | next; });
}

int amoand_w(int* addr, int value) {
  return atomic_fetch_op(addr, value, [](int current, int next) { return current & next; });
}

int amoxor_w(int* addr, int value) {
  return atomic_fetch_op(addr, value, [](int current, int next) { return current ^ next; });
}

int amomax_w(int* addr, int value) {
  return atomic_fetch_op(addr, value, [](int current, int next) { return std::max(current, next); });
}

int amomin_w(int* addr, int value) {
  return atomic_fetch_op(addr, value, [](int current, int next) { return std::min(current, next); });
}

unsigned int amomaxu_w(unsigned int* addr, unsigned int value) {
  std::atomic_ref<unsigned int> atomic_ref(*addr);
  unsigned int current = atomic_ref.load(std::memory_order_relaxed);
  while (!atomic_ref.compare_exchange_weak(current,
                                           std::max(current, value),
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
  }
  return current;
}

unsigned int amominu_w(unsigned int* addr, unsigned int value) {
  std::atomic_ref<unsigned int> atomic_ref(*addr);
  unsigned int current = atomic_ref.load(std::memory_order_relaxed);
  while (!atomic_ref.compare_exchange_weak(current,
                                           std::min(current, value),
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
  }
  return current;
}

}  // namespace hb_native_sim
