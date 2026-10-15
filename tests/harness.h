/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal, dependency-free test harness.
 *
 *   TEST(name) { CHECK(x); CHECK_EQ(a, b); ... }
 *
 * Tests self-register via constructors. Run: test-binary [-v] [-l] [substring-filter...]
 * A failing CHECK records the failure and continues; REQUIRE aborts the current test.
 */
#ifndef ERPS_TEST_HARNESS_H
#define ERPS_TEST_HARNESS_H

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*th_fn)(void);
void th_register(const char *name, th_fn fn, const char *file);
void th_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void th_abort_test(void) __attribute__((noreturn));
int th_verbose(void);
void th_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define TEST(name)                                                                     \
    static void name(void);                                                            \
    __attribute__((constructor)) static void th_reg_##name(void)                       \
    {                                                                                  \
        th_register(#name, name, __FILE__);                                            \
    }                                                                                  \
    static void name(void)

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond))                                                                   \
            th_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);                           \
    } while (0)

#define REQUIRE(cond)                                                                  \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            th_fail(__FILE__, __LINE__, "REQUIRE(%s)", #cond);                         \
            th_abort_test();                                                           \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        long long th_a_ = (long long)(a), th_b_ = (long long)(b);                      \
        if (th_a_ != th_b_)                                                            \
            th_fail(__FILE__, __LINE__, "CHECK_EQ(%s, %s): %lld != %lld", #a, #b,      \
                    th_a_, th_b_);                                                     \
    } while (0)

#define CHECK_NE(a, b)                                                                 \
    do {                                                                               \
        long long th_a_ = (long long)(a), th_b_ = (long long)(b);                      \
        if (th_a_ == th_b_)                                                            \
            th_fail(__FILE__, __LINE__, "CHECK_NE(%s, %s): both %lld", #a, #b, th_a_); \
    } while (0)

#define CHECK_STREQ(a, b)                                                              \
    do {                                                                               \
        const char *th_a_ = (a), *th_b_ = (b);                                         \
        if (strcmp(th_a_, th_b_) != 0)                                                 \
            th_fail(__FILE__, __LINE__, "CHECK_STREQ(%s, %s): \"%s\" != \"%s\"", #a,    \
                    #b, th_a_, th_b_);                                                 \
    } while (0)

#define CHECK_MEMEQ(a, b, n)                                                           \
    do {                                                                               \
        if (memcmp((a), (b), (n)) != 0)                                                \
            th_fail(__FILE__, __LINE__, "CHECK_MEMEQ(%s, %s, %s)", #a, #b, #n);        \
    } while (0)

#endif
