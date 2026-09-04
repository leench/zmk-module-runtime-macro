#pragma once

#define ZMK_TEST_SYS_INIT_NAME(fn) zmk_test_sys_init_##fn
#define ZMK_TEST_SYS_INIT_NAME_INNER(fn) ZMK_TEST_SYS_INIT_NAME(fn)
#define SYS_INIT(fn, level, prio)                                              \
  static int ZMK_TEST_SYS_INIT_NAME_INNER(fn) __attribute__((unused)) = 0
