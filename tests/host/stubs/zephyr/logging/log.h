#pragma once

#define ZMK_TEST_CONCAT_INNER(a, b) a##b
#define ZMK_TEST_CONCAT(a, b) ZMK_TEST_CONCAT_INNER(a, b)
#define LOG_MODULE_DECLARE(...)                                                                    \
    static const int ZMK_TEST_CONCAT(zmk_log_module_dummy_, __LINE__) __attribute__((unused)) = 0
#define LOG_ERR(...) ((void)0)
