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

std::uint64_t snapshot_hash(const volatile void* addr, std::size_t size) {
  auto* bytes = static_cast<const volatile std::uint8_t*>(addr);
  std::uint64_t hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash ^ size;
}

int load_int_prefix(const volatile void* addr, std::size_t size) {
  int value = 0;
  auto* out = reinterpret_cast<std::uint8_t*>(&value);
  auto* bytes = static_cast<const volatile std::uint8_t*>(addr);
  const std::size_t copy_size = std::min(size, sizeof(value));
  for (std::size_t i = 0; i < copy_size; ++i) {
    out[i] = bytes[i];
  }
  return value;
}

void set_core_status(core_context& context, core_status status) {
  context.simulation->statuses[context.core_index].store(status, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->waited_addrs[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->wait_sizes[context.core_index].store(0, std::memory_order_release);
  context.simulation->wait_targets[context.core_index].store(0, std::memory_order_release);
}

void set_wait_word_state(core_context& context, wait_word* waited_word, std::uint64_t target_version) {
  context.simulation->statuses[context.core_index].store(core_status::waiting_wait_word, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(waited_word, std::memory_order_release);
  context.simulation->waited_addrs[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->wait_sizes[context.core_index].store(0, std::memory_order_release);
  context.simulation->wait_targets[context.core_index].store(target_version, std::memory_order_release);
}

void set_memory_wait_state(core_context& context,
                           const volatile void* waited_addr,
                           std::size_t waited_size,
                           std::uint64_t target_snapshot) {
  context.simulation->statuses[context.core_index].store(core_status::waiting_memory, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->waited_addrs[context.core_index].store(waited_addr, std::memory_order_release);
  context.simulation->wait_sizes[context.core_index].store(waited_size, std::memory_order_release);
  context.simulation->wait_targets[context.core_index].store(target_snapshot, std::memory_order_release);
}

void set_barrier_state(core_context& context, std::uint64_t target_generation) {
  context.simulation->statuses[context.core_index].store(core_status::waiting_barrier, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->waited_addrs[context.core_index].store(nullptr, std::memory_order_release);
  context.simulation->wait_sizes[context.core_index].store(0, std::memory_order_release);
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
    case core_status::waiting_wait_word: {
      wait_word* word = simulation.waited_words[core_index].load(std::memory_order_acquire);
      const std::uint64_t target = simulation.wait_targets[core_index].load(std::memory_order_acquire);
      return word != nullptr && word->version.load(std::memory_order_acquire) != target;
    }
    case core_status::waiting_memory: {
      const volatile void* addr = simulation.waited_addrs[core_index].load(std::memory_order_acquire);
      const std::size_t size = simulation.wait_sizes[core_index].load(std::memory_order_acquire);
      const std::uint64_t target = simulation.wait_targets[core_index].load(std::memory_order_acquire);
      return addr != nullptr && snapshot_hash(addr, size) != target;
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

  for (auto& slot : simulation.waited_words) {
    wait_word* word = slot.load(std::memory_order_acquire);
    if (word == nullptr) {
      continue;
    }
    word->version.fetch_add(1, std::memory_order_acq_rel);
    word->version.notify_all();
  }

  for (auto& wake_epoch : simulation.wake_epochs) {
    wake_epoch.fetch_add(1, std::memory_order_acq_rel);
    wake_epoch.notify_all();
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

wait_word& wait_word::operator=(int next) {
  value.store(next, std::memory_order_release);
  version.fetch_add(1, std::memory_order_acq_rel);
  version.notify_all();
  return *this;
}

wait_word::operator int() const {
  return value.load(std::memory_order_acquire);
}

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

int bsg_lr(wait_word* word) {
  core_context& context = current_core_context();
  context.reservation.word = word;
  context.reservation.addr = nullptr;
  context.reservation.size = 0;
  context.reservation.version = word->version.load(std::memory_order_acquire);
  return word->value.load(std::memory_order_acquire);
}

int bsg_lr_aq(wait_word* word) {
  core_context& context = current_core_context();
  const std::uint64_t target = context.reservation.word == word
                                   ? context.reservation.version
                                   : word->version.load(std::memory_order_acquire);
  set_wait_word_state(context, word, target);
  while (word->version.load(std::memory_order_acquire) == target) {
    word->version.wait(target, std::memory_order_acquire);
    throw_if_abort_requested();
  }
  context.reservation = {};
  set_core_status(context, core_status::running);
  return word->value.load(std::memory_order_acquire);
}

int bsg_lr(const volatile void* addr, std::size_t size) {
  core_context& context = current_core_context();
  context.reservation.word = nullptr;
  context.reservation.addr = addr;
  context.reservation.size = size;
  context.reservation.version = snapshot_hash(addr, size);
  return load_int_prefix(addr, size);
}

int bsg_lr_aq(const volatile void* addr, std::size_t size) {
  core_context& context = current_core_context();
  const std::uint64_t target = (context.reservation.word == nullptr &&
                                context.reservation.addr == addr &&
                                context.reservation.size == size)
                                   ? context.reservation.version
                                   : snapshot_hash(addr, size);
  set_memory_wait_state(context, addr, size, target);

  auto& wake_epoch = context.simulation->wake_epochs[context.core_index];
  std::uint64_t observed_epoch = wake_epoch.load(std::memory_order_acquire);
  while (snapshot_hash(addr, size) == target) {
    wake_epoch.wait(observed_epoch, std::memory_order_acquire);
    throw_if_abort_requested();
    observed_epoch = wake_epoch.load(std::memory_order_acquire);
  }

  context.reservation = {};
  set_core_status(context, core_status::running);
  return load_int_prefix(addr, size);
}

void wait_word_store(wait_word& word, int value) {
  word = value;
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

  simulation_state simulation(tile_count);
  for (auto& status : simulation.statuses) {
    status.store(core_status::starting, std::memory_order_relaxed);
  }
  for (auto& waited_word : simulation.waited_words) {
    waited_word.store(nullptr, std::memory_order_relaxed);
  }
  for (auto& waited_addr : simulation.waited_addrs) {
    waited_addr.store(nullptr, std::memory_order_relaxed);
  }
  for (auto& wait_size : simulation.wait_sizes) {
    wait_size.store(0, std::memory_order_relaxed);
  }
  for (auto& wait_target : simulation.wait_targets) {
    wait_target.store(0, std::memory_order_relaxed);
  }
  for (auto& wake_epoch : simulation.wake_epochs) {
    wake_epoch.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> threads;
  threads.reserve(tile_count);
  std::thread watchdog;
  std::exception_ptr thread_error = nullptr;
  std::mutex error_mutex;

  watchdog = std::thread([&] {
    const auto poll_interval = std::chrono::milliseconds(1);

    while (!simulation.abort_requested.load(std::memory_order_acquire)) {
      std::size_t terminated_or_failed = 0;
      bool any_runnable = false;

      for (std::size_t i = 0; i < tile_count; ++i) {
        const core_status status = simulation.statuses[i].load(std::memory_order_acquire);
        if (status == core_status::terminated || status == core_status::failed) {
          ++terminated_or_failed;
        }
        if (status == core_status::waiting_memory) {
          const volatile void* addr = simulation.waited_addrs[i].load(std::memory_order_acquire);
          const std::size_t size = simulation.wait_sizes[i].load(std::memory_order_acquire);
          const std::uint64_t target = simulation.wait_targets[i].load(std::memory_order_acquire);
          if (addr != nullptr && snapshot_hash(addr, size) != target) {
            simulation.wake_epochs[i].fetch_add(1, std::memory_order_acq_rel);
            simulation.wake_epochs[i].notify_all();
          }
        }
        if (core_can_run(simulation, barrier, i)) {
          any_runnable = true;
        }
      }

      if (terminated_or_failed == tile_count) {
        return;
      }

      if (!any_runnable) {
        simulation.deadlock_detected.store(true, std::memory_order_release);
        request_abort(simulation, barrier);
        return;
      }

      std::this_thread::sleep_for(poll_interval);
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
