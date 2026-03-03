#ifndef _BSG_MANYCORE_ATOMIC_H
#define _BSG_MANYCORE_ATOMIC_H

#include "../runtime.hpp"

inline int bsg_amoswap(int* p, int val) {
  return hb_native_sim::amoswap_w(p, val);
}

inline int bsg_amoswap_aq(int* p, int val) {
  return hb_native_sim::amoswap_w(p, val);
}

inline int bsg_amoswap_rl(int* p, int val) {
  return hb_native_sim::amoswap_w(p, val);
}

inline int bsg_amoswap_aqrl(int* p, int val) {
  return hb_native_sim::amoswap_w(p, val);
}

inline int bsg_amoor(int* p, int val) {
  return hb_native_sim::amoor_w(p, val);
}

inline int bsg_amoor_aq(int* p, int val) {
  return hb_native_sim::amoor_w(p, val);
}

inline int bsg_amoor_rl(int* p, int val) {
  return hb_native_sim::amoor_w(p, val);
}

inline int bsg_amoor_aqrl(int* p, int val) {
  return hb_native_sim::amoor_w(p, val);
}

inline int bsg_amoadd(int* p, int val) {
  return hb_native_sim::amoadd_w(p, val);
}

inline int bsg_amoadd_aq(int* p, int val) {
  return hb_native_sim::amoadd_w(p, val);
}

inline int bsg_amoadd_rl(int* p, int val) {
  return hb_native_sim::amoadd_w(p, val);
}

inline int bsg_amoadd_aqrl(int* p, int val) {
  return hb_native_sim::amoadd_w(p, val);
}

#endif
