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

std::chrono::milliseconds deadlock_timeout() {
  const char* env = std::getenv("HAMMER_SIM_DEADLOCK_TIMEOUT_MS");
  if (env == nullptr || *env == '\0') {
    return std::chrono::milliseconds(5000);
  }
  const long value = std::strtol(env, nullptr, 10);
  if (value <= 0) {
    return std::chrono::milliseconds(5000);
  }
  return std::chrono::milliseconds(value);
}

void advance_progress(simulation_state& simulation) {
  simulation.progress_epoch.fetch_add(1, std::memory_order_acq_rel);
}

void set_core_status(core_context& context, core_status status, wait_word* waited_word = nullptr) {
  context.simulation->statuses[context.core_index].store(status, std::memory_order_release);
  context.simulation->waited_words[context.core_index].store(waited_word, std::memory_order_release);
  advance_progress(*context.simulation);
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

void barrier_t::arrive_and_wait() {
  const std::size_t gen = generation.load(std::memory_order_acquire);
  const std::size_t prior = arrived.fetch_add(1, std::memory_order_acq_rel);
  if (prior + 1 == participants) {
    arrived.store(0, std::memory_order_release);
    generation.fetch_add(1, std::memory_order_acq_rel);
    generation.notify_all();
    return;
  }
  while (generation.load(std::memory_order_acquire) == gen) {
    generation.wait(gen, std::memory_order_acquire);
  }
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
  context.reservation.version = word->version.load(std::memory_order_acquire);
  return word->value.load(std::memory_order_acquire);
}

int bsg_lr_aq(wait_word* word) {
  core_context& context = current_core_context();
  set_core_status(context, core_status::waiting_wait_word, word);
  const std::uint64_t target = context.reservation.word == word
                                   ? context.reservation.version
                                   : word->version.load(std::memory_order_acquire);
  while (word->version.load(std::memory_order_acquire) == target) {
    word->version.wait(target, std::memory_order_acquire);
    throw_if_abort_requested();
  }
  context.reservation = {};
  set_core_status(context, core_status::running);
  return word->value.load(std::memory_order_acquire);
}

void wait_word_store(wait_word& word, int value) {
  word = value;
}

void barrier_init() {
}

void barrier_sync() {
  core_context& context = current_core_context();
  set_core_status(context, core_status::waiting_barrier);
  context.barrier->arrive_and_wait();
  throw_if_abort_requested();
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

  std::vector<std::thread> threads;
  threads.reserve(tile_count);
  std::thread watchdog;
  std::exception_ptr thread_error = nullptr;
  std::mutex error_mutex;

  watchdog = std::thread([&] {
    const auto timeout = deadlock_timeout();
    const auto poll_interval = std::chrono::milliseconds(10);
    std::uint64_t last_epoch = simulation.progress_epoch.load(std::memory_order_acquire);
    auto stalled_since = std::chrono::steady_clock::now();

    while (!simulation.abort_requested.load(std::memory_order_acquire)) {
      std::size_t running_or_starting = 0;
      std::size_t terminated_or_failed = 0;

      for (auto& status_slot : simulation.statuses) {
        const core_status status = status_slot.load(std::memory_order_acquire);
        if (status == core_status::starting || status == core_status::running) {
          ++running_or_starting;
        }
        if (status == core_status::terminated || status == core_status::failed) {
          ++terminated_or_failed;
        }
      }

      if (terminated_or_failed == tile_count) {
        return;
      }

      const std::uint64_t epoch = simulation.progress_epoch.load(std::memory_order_acquire);
      if (running_or_starting != 0 || epoch != last_epoch) {
        last_epoch = epoch;
        stalled_since = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - stalled_since >= timeout) {
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
