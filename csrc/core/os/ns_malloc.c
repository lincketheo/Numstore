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

#include "core/os/ns_malloc.h"

#include "core/ns_bounds.h"
#include "core/ns_csx_assert.h"

#ifndef NDEBUG
#  include "core/ns_testing.h" // TEST
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void *
default_malloc (void *self, const u32 nelem, const u32 size, error *e)
{
  ASSERT (self == NULL);
  ASSERT (nelem > 0);
  ASSERT (size > 0);

  u32 bytes;
  if (!safe_mul_u32 (&bytes, nelem, size)) {
    error_causef (e, ERR_NOMEM, "malloc %u*%u: overflow", nelem, size);
    return NULL;
  }

  errno     = 0;
  void *ret = malloc ((size_t)bytes);
  if (ret == NULL) {
    if (errno == ENOMEM) {
      error_causef (e, ERR_NOMEM, "malloc %u*%u: %s", nelem, size, strerror (errno));
    } else {
      error_causef (e, ERR_NOMEM, "malloc %u*%u: out of memory", nelem, size);
    }
  }
  return ret;
}

static void *
default_calloc (void *self, const u32 nelem, const u32 size, error *e)
{
  ASSERT (self == NULL);
  ASSERT (nelem > 0);
  ASSERT (size > 0);

  u32 bytes = 0;
  if (!safe_mul_u32 (&bytes, nelem, size)) {
    error_causef (e, ERR_NOMEM, "calloc %u*%u: overflow", nelem, size);
    return NULL;
  }

  ASSERT (bytes > 0);

  errno     = 0;
  void *ret = calloc ((size_t)nelem, (size_t)size);
  if (ret == NULL) {
    if (errno == ENOMEM) {
      error_causef (e, ERR_NOMEM, "calloc %u*%u: %s", nelem, size, strerror (errno));
    } else {
      error_causef (e, ERR_NOMEM, "calloc %u*%u: out of memory", nelem, size);
    }
  }
  return ret;
}

static void *
default_realloc (void *self, void *ptr, const u32 nelem, const u32 size, error *e)
{
  ASSERT (self == NULL);
  ASSERT (nelem > 0);
  ASSERT (size > 0);

  u32 bytes = 0;
  if (!safe_mul_u32 (&bytes, nelem, size)) {
    error_causef (e, ERR_NOMEM, "realloc %u*%u: overflow", nelem, size);
    return NULL;
  }

  errno     = 0;
  void *ret = realloc (ptr, (size_t)bytes);
  if (ret == NULL) {
    error_causef (e, ERR_NOMEM, "realloc %u bytes: %s", bytes, strerror (errno));
    return NULL;
  }
  return ret;
}

static void
default_free (void *self, void *v)
{
  ASSERT (self == NULL);
  ASSERT (v);
  free (v);
}

static const struct i_mem_vtable default_mem_vtable = {
    .malloc  = default_malloc,
    .calloc  = default_calloc,
    .realloc = default_realloc,
    .free    = default_free,
};

struct i_mem
default_mem (void)
{
  return (struct i_mem){
      .self  = NULL,
      .table = &default_mem_vtable,
  };
}

#ifndef NDEBUG

/******************************************************************************
 * SECTION: Tests
 ******************************************************************************/

/// Every entry point multiplies nelem*size into a u32, so the guard is the
/// only thing between a caller and a short buffer. Each row is a product that
/// does not fit:
///
///   [ nelem ] * [ size ] -> > UINT32_MAX
///
/// realloc gets a live block so a missed guard would shrink it, not just
/// return NULL.
#  define OVERFLOW_GUARD_CASE(nelem, size)                                                   \
    do {                                                                                     \
      TEST_CASE ("%s * %s overflows", #nelem, #size)                                         \
      {                                                                                      \
        struct i_mem m = default_mem ();                                                     \
                                                                                             \
        test_assert_ptr_equal (m.table->malloc (m.self, nelem, size, &ctx->e), NULL);        \
        test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);                                \
        error_reset (&ctx->e);                                                               \
                                                                                             \
        test_assert_ptr_equal (m.table->calloc (m.self, nelem, size, &ctx->e), NULL);        \
        test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);                                \
        error_reset (&ctx->e);                                                               \
                                                                                             \
        void *live = m.table->malloc (m.self, 1, 8, &ctx->e);                                \
        test_assert (live != NULL);                                                          \
        test_assert_ptr_equal (m.table->realloc (m.self, live, nelem, size, &ctx->e), NULL); \
        test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);                                \
        error_reset (&ctx->e);                                                               \
        m.table->free (m.self, live);                                                        \
      }                                                                                      \
    }                                                                                        \
    while (0)

TEST (default_mem_overflow_guards)
{
  OVERFLOW_GUARD_CASE (0x10000u, 0x10000u); // exactly 2^32
  OVERFLOW_GUARD_CASE (0x10000u, 0x10001u);
  OVERFLOW_GUARD_CASE (UINT32_MAX, 2u);
  OVERFLOW_GUARD_CASE (2u, UINT32_MAX);
  OVERFLOW_GUARD_CASE (UINT32_MAX, UINT32_MAX);
}

/// The largest product that still fits must be allowed through, so the guard
/// is not off by one.
TEST (default_mem_allocates)
{
  struct i_mem m = default_mem ();

  TEST_CASE ("malloc hands back nelem*size writable bytes")
  {
    u8 *p = m.table->malloc (m.self, 16, 4, &ctx->e);
    test_assert (p != NULL);
    memset (p, 0xab, 16 * 4);
    test_assert_int_equal (p[0], 0xab);
    test_assert_int_equal (p[63], 0xab);
    test_assert_int_equal (ctx->e.cause_code, SUCCESS);
    m.table->free (m.self, p);
  }

  TEST_CASE ("calloc zeroes")
  {
    u8 *p = m.table->calloc (m.self, 32, 2, &ctx->e);
    test_assert (p != NULL);
    for (u32 i = 0; i < 64; ++i) {
      test_assert_int_equal (p[i], 0);
    }
    m.table->free (m.self, p);
  }

  TEST_CASE ("realloc preserves the prefix it grew from")
  {
    u8 *p = m.table->malloc (m.self, 8, 1, &ctx->e);
    test_assert (p != NULL);
    memset (p, 0x5a, 8);

    u8 *grown = m.table->realloc (m.self, p, 64, 1, &ctx->e);
    test_assert (grown != NULL);
    for (u32 i = 0; i < 8; ++i) {
      test_assert_int_equal (grown[i], 0x5a);
    }
    m.table->free (m.self, grown);
  }

  TEST_CASE ("UINT32_MAX * 1 passes the guard")
  {
    // The product fits, so the guard must not reject it. The host is free to
    // refuse 4GiB; what matters is that the refusal came from the allocator.
    void *p = m.table->malloc (m.self, UINT32_MAX, 1, &ctx->e);
    if (p == NULL) {
      test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);
      error_reset (&ctx->e);
    } else {
      m.table->free (m.self, p);
    }
  }
}

/// The faulty decorator is the only way a test can make an allocation fail on
/// demand, so its budget has to land on the call the test aimed at.
TEST (faulty_mem_fail_after_budget)
{
  struct i_mem f = ctx->faulty_mem;

  TEST_CASE ("budget 0 fails the next call")
  {
    faulty_mem_fail_after (f, 0);
    test_assert_ptr_equal (f.table->malloc (f.self, 1, 8, &ctx->e), NULL);
    test_assert (ctx->e.cause_code < 0);
    error_reset (&ctx->e);
  }

  TEST_CASE ("budget N lets N calls through, then fails once")
  {
    faulty_mem_reset (f);
    faulty_mem_fail_after (f, 2);

    void *a = f.table->malloc (f.self, 1, 8, &ctx->e);
    void *b = f.table->malloc (f.self, 1, 8, &ctx->e);
    test_assert (a != NULL);
    test_assert (b != NULL);

    test_assert_ptr_equal (f.table->malloc (f.self, 1, 8, &ctx->e), NULL);
    test_assert (ctx->e.cause_code < 0);
    error_reset (&ctx->e);

    // The budget is one-shot: it disarms itself after firing.
    void *c = f.table->malloc (f.self, 1, 8, &ctx->e);
    test_assert (c != NULL);

    f.table->free (f.self, a);
    f.table->free (f.self, b);
    f.table->free (f.self, c);
  }

  faulty_mem_reset (f);
}

#endif // NDEBUG
