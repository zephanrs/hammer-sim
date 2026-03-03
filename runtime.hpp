#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace hb_native_sim {

using eva_t = std::uint32_t;

struct reservation_t {
  const volatile void* addr = nullptr;
  struct reservation_word_t* word = nullptr;
  std::uint64_t epoch = 0;
};

struct reservation_word_t {
  std::atomic<std::uint64_t> epoch{0};
};

enum class core_status : std::uint8_t {
  starting = 0,
  running = 1,
  waiting_memory = 2,
  waiting_barrier = 3,
  terminated = 4,
  failed = 5,
};

struct barrier_t {
  void init(std::size_t participants);

  std::size_t participants = 0;
  std::atomic<std::size_t> arrived{0};
  std::atomic<std::size_t> generation{0};
};

struct simulation_state {
  explicit simulation_state(std::size_t core_count, std::size_t scratchpad_word_count)
      : statuses(core_count), waited_words(core_count), wait_targets(core_count),
        scratchpad_words(scratchpad_word_count), scratchpad_mutexes(core_count) {}

  std::vector<std::atomic<core_status>> statuses;
  std::vector<std::atomic<reservation_word_t*>> waited_words;
  std::vector<std::atomic<std::uint64_t>> wait_targets;
  std::vector<reservation_word_t> scratchpad_words;
  std::vector<std::mutex> scratchpad_mutexes;
  void* scratchpad_block = nullptr;
  std::size_t scratchpad_size = 0;
  std::size_t words_per_tile = 0;
  std::atomic<bool> abort_requested{false};
  std::atomic<bool> deadlock_detected{false};
};

struct kernel_descriptor {
  using invoke_fn = int (*)(const std::uint32_t*);
  using construct_fn = void (*)(void*, std::size_t);
  using destroy_fn = void (*)(void*, std::size_t);

  std::string name;
  invoke_fn invoke = nullptr;
  std::size_t scratchpad_size = 0;
  std::size_t scratchpad_align = alignof(std::max_align_t);
  construct_fn construct = nullptr;
  destroy_fn destroy = nullptr;
};

struct kernel_launch {
  std::string name;
  std::vector<std::uint32_t> argv;
  std::uint32_t tile_group_x = 0;
  std::uint32_t tile_group_y = 0;
};

struct manycore_t {};

struct device_state {
  std::vector<std::uint8_t> memory;
  eva_t next_alloc = 0;
  std::vector<kernel_launch> launches;
};

struct core_context {
  std::size_t core_index = 0;
  int x = 0;
  int y = 0;
  void* scratchpad_base = nullptr;
  void* scratchpad_block = nullptr;
  std::size_t scratchpad_size = 0;
  std::size_t tile_group_x = 0;
  std::size_t tile_group_y = 0;
  barrier_t* barrier = nullptr;
  device_state* device = nullptr;
  simulation_state* simulation = nullptr;
  reservation_t reservation;
};

void register_program_main(int (*fn)(int, char**));
int (*program_main())(int, char**);

void register_kernel(kernel_descriptor descriptor);
const kernel_descriptor& lookup_kernel(const std::string& name);

void set_current_core_context(core_context* context);
core_context& current_core_context();
void* current_scratchpad_base();
void* remote_ptr(int x, int y, const void* local_ptr);
void* eva_to_ptr(eva_t addr);

int current_x();
int current_y();

int bsg_lr(const volatile void* addr);
int bsg_lr_aq(const volatile void* addr);
void load_bytes(const volatile void* addr, void* out, std::size_t size);
void store_bytes(void* addr, const void* value, std::size_t size);
void store_bytes(volatile void* addr, const void* value, std::size_t size);
void store_int_word(int* addr, int value);
void store_int_word(volatile int* addr, int value);

template <typename T>
inline std::remove_cv_t<T> load(const T* addr) {
  using value_type = std::remove_cv_t<T>;
  value_type value{};
  load_bytes(static_cast<const volatile void*>(addr), &value, sizeof(value_type));
  return value;
}

template <typename T>
inline std::remove_cv_t<T> load(const volatile T* addr) {
  using value_type = std::remove_cv_t<T>;
  value_type value{};
  load_bytes(static_cast<const volatile void*>(addr), &value, sizeof(value_type));
  return value;
}

template <typename T, typename U>
inline void store(T* addr, U&& value) {
  using value_type = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<value_type, int>) {
    store_int_word(addr, static_cast<int>(std::forward<U>(value)));
  } else {
    const value_type typed_value = static_cast<value_type>(std::forward<U>(value));
    store_bytes(addr, &typed_value, sizeof(value_type));
  }
}

template <typename T, typename U>
inline void store(volatile T* addr, U&& value) {
  using value_type = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<value_type, int>) {
    store_int_word(addr, static_cast<int>(std::forward<U>(value)));
  } else {
    const value_type typed_value = static_cast<value_type>(std::forward<U>(value));
    store_bytes(addr, &typed_value, sizeof(value_type));
  }
}

void barrier_init();
void barrier_sync();
void kernel_start();
void kernel_end();
void fence();

int run_kernel(device_state& device, const kernel_launch& launch);

int amoadd_w(int* addr, int value);
int amoswap_w(int* addr, int value);
int amoor_w(int* addr, int value);
int amoand_w(int* addr, int value);
int amoxor_w(int* addr, int value);
int amomax_w(int* addr, int value);
int amomin_w(int* addr, int value);
unsigned int amomaxu_w(unsigned int* addr, unsigned int value);
unsigned int amominu_w(unsigned int* addr, unsigned int value);

}  // namespace hb_native_sim
