/// Copyright 2026 Theo Lincke
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.

#ifndef TESTING_H
#define TESTING_H

#include <inttypes.h>
#include <stdbool.h>
#ifndef NDEBUG
#  include "core/ns_arena_alloc.h" // struct arena_alloc
#  include "core/ns_csx_assert.h"  // ASSERT
#  include "core/ns_error.h"       // error, error_create
#  include "core/ns_logging.h"     // i_log_info
#  include "core/ns_platform.h"    // HEADER_FUNC
#  include "core/ns_utils.h"       // FPREFIX_STR
#  include "core/os/ns_malloc.h"   // struct i_mem, default_mem
#  include "core/os/ns_os.h"       // struct i_os, system_os_create

extern int test_ret;

// Limits
enum
{
  test_faults_max = 100,
  test_marks_max  = 100,
  test_mark_len   = 100,
};

/******************************************************************************
 * SECTION: TEST Macro
 * ----------------------------------------------------------------------------
 * @brief Macro for defining new tests
 ******************************************************************************/

/// Every test body gets a [ctx] holding an error, the process allocator, a
/// fresh system OS, faulty decorators over both, and an arena over each. It is
/// freed on the way out, which also asserts that the test left no file open
/// and no thread unjoined.
struct test_ctx
{
  error              e;
  struct i_mem       mem;
  struct i_os        os;
  struct i_mem       faulty_mem;
  struct i_os        faulty_os;
  struct arena_alloc alloc;
  struct arena_alloc faulty_alloc;
};

struct test_ctx *test_ctx_create (error *e);
void test_ctx_free (struct test_ctx *ctx);

#  define TEST_BODY_DECL(name) static void __test_body__##name (struct test_ctx *ctx);

#  define TEST_BODY_IMPL(name) static void __test_body__##name (struct test_ctx *ctx)

#  define TEST_NOOP(name) \
    MAYBE_UNUSED static void __disabled_test_body__##name (struct test_ctx *ctx)

#  define TEST_WRAPPER(name)                                   \
    void __test__##name (void)                                 \
    {                                                          \
      error            ___e = error_create ();                 \
      struct test_ctx *ctx  = test_ctx_create (&___e);         \
      if (ctx == NULL) {                                       \
        i_log_failure ("%s: test_ctx_create failed\n", #name); \
        test_ret = -1;                                         \
        return;                                                \
      }                                                        \
      __test_body__##name (ctx);                               \
      test_ctx_free (ctx);                                     \
    }

#  define TEST_DISABLED(name) \
    TEST_BODY_DECL (name)     \
    TEST_WRAPPER (name)       \
    TEST_BODY_IMPL (name)     \
    { /* noop */              \
    }                         \
    TEST_NOOP (name)

#  define TEST(name)      \
    TEST_BODY_DECL (name) \
    TEST_WRAPPER (name)   \
    TEST_BODY_IMPL (name)

#  define TEST_CASE(fmt, ...)                                                                \
    for (int _tc_once = (i_log_test_case (fmt "\n", ##__VA_ARGS__), 1), _tc_prev = test_ret; \
         _tc_once;                                                                           \
         _tc_once = 0,                                                                       \
             (test_ret == _tc_prev ? (i_log_passed ("------ : " fmt "\n", ##__VA_ARGS__), 0) \
                                   : (i_log_failure ("------ : " fmt "\n", ##__VA_ARGS__), 0)))

/******************************************************************************
 * SECTION: Fault Injection
 * ----------------------------------------------------------------------------
 * @brief injecting faults
 ******************************************************************************/

bool fault_is_set (const char *name);
void fault_set (const char *name);
void fault_reset_all (void);

#  ifndef FAULT
#    define FAULT(expr, name)                                                                \
      (fault_is_set (name) ? (error_causef ((e), ERR_INVALID_ARGUMENT, "Fault: %s", (name))) \
                           : (expr))

#    define FAULT_NULL(expr, name)                                                      \
      (fault_is_set (name)                                                              \
           ? (error_causef ((e), ERR_INVALID_ARGUMENT, "FAULT: %s", (name)), (void *)0) \
           : (expr))
#  endif

/******************************************************************************
 * SECTION: TEST Marker
 * ----------------------------------------------------------------------------
 * @brief A way to check if a line of code was run
 ******************************************************************************/

extern char test_marks[test_marks_max][test_mark_len];
extern int  test_marks_count;

// Match a test marker string
HEADER_FUNC bool
test_mark_match (const char *pat, const char *str)
{
  while (*pat) {
    if (*pat == '*') {
      pat++;
      if (!*pat) {
        return true;
      }

      while (*str) {
        if (test_mark_match (pat, str)) {
          return true;
        }
        str++;
      }
      return false;
    }
    if (*str != *pat) {
      return false;
    }
    pat++;
    str++;
  }
  return *str == '\0';
}

HEADER_FUNC void
__test_mark (const char *_src)
{
  ASSERT (test_marks_count < test_marks_max);
  char *_dst = test_marks[test_marks_count++];
  int   _i   = 0;

  while (_i < test_mark_len - 1 && _src[_i]) {
    _dst[_i] = _src[_i];
    _i++;
  }
  _dst[_i] = '\0';
}

#  define TEST_MARK(label)            \
    do {                              \
      if (!__test_mark_hit (label)) { \
        __test_mark (label);          \
      }                               \
    }                                 \
    while (0)

#  define test_reset_marks() \
    do {                     \
      test_marks_count = 0;  \
    }                        \
    while (0)

/******************************************************************************
 * SECTION: TEST runtime checks
 * ----------------------------------------------------------------------------
 * @brief methods to use during tests
 ******************************************************************************/

#  define fail_test(fmt, ...)                                     \
    do {                                                          \
      i_log_failure (FPREFIX_STR fmt, FPREFIX_ARGS, __VA_ARGS__); \
      test_ret = -1;                                              \
      return;                                                     \
    }                                                             \
    while (0)

#  define test_assert_equal(left, right)         \
    do {                                         \
      if ((left) != (right)) {                   \
        fail_test ("%s != %s\n", #left, #right); \
      }                                          \
    }                                            \
    while (0)

#  define test_assert_int_equal(left, right) test_assert_type_equal (left, right, i32, PRId32)

#  define test_assert_type_equal(left, right, type, fmt) \
    do {                                                 \
      type _left  = left;                                \
      type _right = right;                               \
      if ((_left) != (_right)) {                         \
        fail_test (                                      \
            "Expression: %s != %s values: "              \
            "(%" fmt ") != (%" fmt ")\n",                \
            #left,                                       \
            #right,                                      \
            _left,                                       \
            _right                                       \
        );                                               \
      }                                                  \
    }                                                    \
    while (0)

#  define test_assert_ptr_equal(left, right) test_assert_equal ((void *)left, (void *)right)

#  define test_assert(expr)                  \
    do {                                     \
      if (!(expr)) {                         \
        fail_test ("Expected: %s\n", #expr); \
      }                                      \
    }                                        \
    while (0)

#  define test_fail_if(expr)                   \
    do {                                       \
      if (expr) {                              \
        fail_test ("Unexpected: %s\n", #expr); \
      }                                        \
    }                                          \
    while (0)

#  define test_err_t_check(expr, exp, ename) \
    do {                                     \
      err_t __ret = (err_t)expr;             \
      test_assert_int_equal (__ret, exp);    \
      error_reset (ename);                   \
    }                                        \
    while (0)

#  define test_assert_memequal(a, b, size) test_assert_int_equal (memcmp (a, b, size), 0)

HEADER_FUNC bool
__test_mark_hit (const char *_pat)
{
  bool _hit = false;
  for (int _i = 0; _i < test_marks_count; _i++) {
    if (test_mark_match (_pat, test_marks[_i])) {
      _hit = true;
      break;
    }
  }
  return _hit;
}

#  define test_assert_mark_hit(pattern)                       \
    do {                                                      \
      if (!__test_mark_hit (pattern)) {                       \
        fail_test ("No mark matched pattern: %s\n", pattern); \
      }                                                       \
    }                                                         \
    while (0)

#  define test_assert_mark_not_hit(pattern)                \
    do {                                                   \
      if (__test_mark_hit (pattern)) {                     \
        fail_test ("Mark matched pattern: %s\n", pattern); \
      }                                                    \
    }                                                      \
    while (0)

#else
#  define TEST_MARK(v)
#endif
#endif // TESTING_H
