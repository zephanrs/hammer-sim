#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hb_native_sim {

using eva_t = std::uint32_t;

struct wait_word {
  wait_word(int initial = 0) : value(initial), version(1) {}
  wait_word(const wait_word&) = delete;
  wait_word& operator=(const wait_word&) = delete;

  wait_word& operator=(int next);
  operator int() const;

  std::atomic<int> value;
  std::atomic<std::uint64_t> version;
};

struct reservation_t {
  wait_word* word = nullptr;
  std::uint64_t version = 0;
};

enum class core_status : std::uint8_t {
  starting = 0,
  running = 1,
  waiting_wait_word = 2,
  waiting_barrier = 3,
  terminated = 4,
  failed = 5,
};

struct barrier_t {
  void init(std::size_t participants);
  void arrive_and_wait();

  std::size_t participants = 0;
  std::atomic<std::size_t> arrived{0};
  std::atomic<std::size_t> generation{0};
};

struct simulation_state {
  explicit simulation_state(std::size_t core_count)
      : statuses(core_count), waited_words(core_count) {}

  std::vector<std::atomic<core_status>> statuses;
  std::vector<std::atomic<wait_word*>> waited_words;
  std::atomic<bool> abort_requested{false};
  std::atomic<bool> deadlock_detected{false};
  std::atomic<std::uint64_t> progress_epoch{0};
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

int bsg_lr(wait_word* word);
int bsg_lr_aq(wait_word* word);
void wait_word_store(wait_word& word, int value);

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
