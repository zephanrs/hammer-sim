#pragma once

#include <cstdint>

#ifdef HB_NATIVE_SIM
#include "../runtime.hpp"
#endif

static inline int hb_amoadd_w(int* addr, int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amoadd_w(addr, value);
#else
  int old = 0;
  asm volatile("amoadd.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline int hb_amoswap_w(int* addr, int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amoswap_w(addr, value);
#else
  int old = 0;
  asm volatile("amoswap.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline int hb_amoor_w(int* addr, int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amoor_w(addr, value);
#else
  int old = 0;
  asm volatile("amoor.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline int hb_amoand_w(int* addr, int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amoand_w(addr, value);
#else
  int old = 0;
  asm volatile("amoand.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline int hb_amoxor_w(int* addr, int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amoxor_w(addr, value);
#else
  int old = 0;
  asm volatile("amoxor.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline int hb_amomax_w(int* addr, int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amomax_w(addr, value);
#else
  int old = 0;
  asm volatile("amomax.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline int hb_amomin_w(int* addr, int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amomin_w(addr, value);
#else
  int old = 0;
  asm volatile("amomin.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline unsigned int hb_amomaxu_w(unsigned int* addr, unsigned int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amomaxu_w(addr, value);
#else
  unsigned int old = 0;
  asm volatile("amomaxu.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}

static inline unsigned int hb_amominu_w(unsigned int* addr, unsigned int value) {
#ifdef HB_NATIVE_SIM
  return hb_native_sim::amominu_w(addr, value);
#else
  unsigned int old = 0;
  asm volatile("amominu.w %0, %2, (%1)"
               : "=r"(old)
               : "r"(addr), "r"(value)
               : "memory");
  return old;
#endif
}
