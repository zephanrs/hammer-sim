#pragma once

#include "../runtime.hpp"

#define declare_program_main(name_literal, fn)                                  \
  namespace {                                                                   \
  const bool hb_native_program_registered_##fn = []() {                         \
    hb_native_sim::register_program_main(fn);                                   \
    return true;                                                                \
  }();                                                                          \
  }
