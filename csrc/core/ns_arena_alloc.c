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

#include "core/ns_arena_alloc.h"

#include "core/ns_bounds.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_latch.h"
#include "core/os/ns_malloc.h"

#ifndef NDEBUG
#  include "core/ns_numerics.h" // randu32r
#  include "core/ns_testing.h"  // TEST
#endif

#include <stdint.h>
#include <string.h>

/******************************************************************************
 * SECTION: Local Linear Allocator
 ******************************************************************************/

/**
 * @struct lalloc
 * @brief A local arena allocator
 *
 * An allocator that allocates from a fixed size buffer
 * provided by the user
 *
 * @var lalloc::latch
 * @brief The latch to maintain thread safety
 *
 * @var lalloc::used
 * @brief How many bytes have been used
 *
 * @var lalloc::limit
 * @brief The maximum number of bytes available
 *
 * @var lalloc::data
 * @brief The buffer that holds all the data
 */
struct lalloc
{
  latch latch;
  u32   used;
  u32   limit;
  u8   *data;
};

DEFINE_DBG_ASSERT (struct lalloc, lalloc, l, {
  ASSERT (l);
  ASSERT (l->data);
  ASSERT (l->used <= l->limit);
})

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static struct lalloc
lalloc_create (u8 *data, const u32 limit)
{
  ASSERT (limit > 0);
  struct lalloc ret = {
      .used  = 0,
      .limit = limit,
      .data  = data,
  };
  latch_init (&ret.latch);
  DBG_ASSERT (lalloc, &ret);
  return ret;
}

static void *
lmalloc (struct lalloc *a, const u32 req, const u32 size, error *e)
{
  latch_lock (&a->latch);

  DBG_ASSERT (lalloc, a);
  ASSERT (req > 0);
  ASSERT (size > 0);

  u32 total;
  if (!safe_mul_u32 (&total, req, size)) {
    error_causef (e, ERR_NOMEM, "alloc %d*%d: overflow", req, size);
    latch_unlock (&a->latch);
    return NULL;
  }

  const u32 avail = a->limit - a->used;
  if (avail < total) {
    error_causef (e, ERR_NOMEM, "linear alloc %d bytes: only %d remaining", total, avail);
    latch_unlock (&a->latch);
    return NULL;
  }

  void *ret = &a->data[a->used];
  a->used   = a->used + total;

  latch_unlock (&a->latch);

  return ret;
}

/******************************************************************************
 * SECTION: Chunk Allocator
 ******************************************************************************/

/**
 * @struct chunk
 * @brief Single link block within a chunk allocator chain
 *
 * Wraps a standard local linear allocator instance alongside a flexible data
 * array which handles the payload tracking for this specific segment.
 *
 * @var chunk::alloc
 * @brief The internal linear allocator wrapper riding on top of the chunk data.
 * @var chunk::next
 * @brief Pointer to the subsequent chunk link in the chain or NULL if tail.
 * @var chunk::data
 * @brief Inline flexible array handling the raw bytes owned by this block.
 */
struct chunk
{
  struct lalloc alloc;  // Base allocator interface for this chunk
  struct chunk *next;   // Next chunk in the linked list, or NULL if tail
  u8            data[]; // Flexible array of chunk-owned bytes
};

DEFINE_DBG_ASSERT (struct chunk, chunk, c, {
  ASSERT (c);
  ASSERT (c->alloc.data == c->data);
  ASSERT (c->alloc.used <= c->alloc.limit);
  ASSERT (c->alloc.limit > 0);
  ASSERT (c->alloc.data == (u8 *)c + sizeof (struct chunk));
})

void
arena_alloc_assert (const struct arena_alloc *ca)
{
  ASSERT (ca);
  ASSERT (ca->settings.target_chunk_mult >= 1.0f);
  ASSERT (ca->settings.min_chunk_size > 0);
  ASSERT (
      ca->settings.max_chunk_size == 0 || ca->settings.max_chunk_size >= ca->settings.min_chunk_size
  );
  ASSERT (ca->head != NULL || ca->num_chunks == 0);
  ASSERT (ca->total_used <= ca->total_allocated);
  ASSERT (ca->settings.max_total_size == 0 || ca->total_allocated <= ca->settings.max_total_size);
  ASSERT (ca->settings.max_chunks == 0 || ca->num_chunks <= ca->settings.max_chunks);

  u32 counted_chunks    = 0;
  u32 counted_allocated = 0;
  u32 counted_used      = 0;

  for (const struct chunk *c = ca->head; c != NULL; c = c->next) {
    DBG_ASSERT (chunk, c);

    counted_chunks++;
    counted_allocated += c->alloc.limit;
    counted_used += c->alloc.used;

    ASSERT (counted_chunks <= ca->settings.max_chunks || ca->settings.max_chunks == 0);
    ASSERT (counted_chunks <= 100000);
  }

  // Verify counts match
  ASSERT (counted_chunks == ca->num_chunks);
  ASSERT (counted_allocated == ca->total_allocated);
  ASSERT (counted_used == ca->total_used);
}

static struct chunk *
chunk_create (const u32 size, struct i_mem mem, error *e)
{
  struct chunk *ret = mem.table->malloc (mem.self, sizeof (struct chunk) + size, 1, e);
  if (ret == NULL) {
    return NULL;
  }
  ret->alloc = lalloc_create (ret->data, size);
  ret->next  = NULL;
  DBG_ASSERT (chunk, ret);
  return ret;
}

void
arena_alloc_create (struct arena_alloc *dest, const struct arena_alloc_settings settings)
{
  ASSERT (settings.target_chunk_mult >= 1.0f);
  ASSERT (settings.min_chunk_size > 0);
  ASSERT (settings.max_chunk_size == 0 || settings.max_chunk_size >= settings.min_chunk_size);

  *dest = (struct arena_alloc){
      .settings        = settings,
      .head            = NULL,
      .num_chunks      = 0,
      .total_allocated = 0,
      .total_used      = 0,
  };

  latch_init (&dest->latch);

  DBG_ASSERT (arena_alloc, dest);
}

void
arena_alloc_create_default_with (struct arena_alloc *dest, struct i_mem mem)
{
  arena_alloc_create (
      dest,
      (struct arena_alloc_settings){
          .max_alloc_size    = 0,
          .max_total_size    = 0,
          .target_chunk_mult = 10,
          .min_chunk_size    = 10,
          .max_chunk_size    = 0,
          .max_chunks        = 0,
          .mem               = mem,
      }
  );
}

void
arena_alloc_create_default (struct arena_alloc *dest)
{
  arena_alloc_create (
      dest,
      (struct arena_alloc_settings){
          .max_alloc_size    = 0,
          .max_total_size    = 0,
          .target_chunk_mult = 10,
          .min_chunk_size    = 10,
          .max_chunk_size    = 0,
          .max_chunks        = 0,
          .mem               = default_mem (),
      }
  );
}

static u32
compute_new_chunk_size (const struct arena_alloc *ca, const u32 alloc_size)
{
  DBG_ASSERT (arena_alloc, ca);

  // Target chunk size based on multiplier
  u32 new_chunk_size = (u32)(alloc_size * ca->settings.target_chunk_mult);

  // Clamp to minimum
  if (new_chunk_size < ca->settings.min_chunk_size) {
    new_chunk_size = ca->settings.min_chunk_size;
  }

  // Clamp to maximum
  if (ca->settings.max_chunk_size > 0 && new_chunk_size > ca->settings.max_chunk_size) {
    new_chunk_size = ca->settings.max_chunk_size;
  }

  // Ensure it fits the current allocation
  if (new_chunk_size < alloc_size) {
    new_chunk_size = alloc_size;
  }

  return new_chunk_size;
}

void
arena_alloc_free_all (struct arena_alloc *ca)
{
  latch_lock (&ca->latch);

  DBG_ASSERT (arena_alloc, ca);

  struct chunk *cur = ca->head;
  while (cur != NULL) {
    struct chunk *next = cur->next;
    DBG_ASSERT (chunk, cur);
    ca->settings.mem.table->free (ca->settings.mem.self, cur);
    cur = next;
  }

  ca->head            = NULL;
  ca->num_chunks      = 0;
  ca->total_allocated = 0;
  ca->total_used      = 0;

  latch_unlock (&ca->latch);
}

void *
arena_alloc_copy (struct arena_alloc *alloc, const void *ptr, u32 size, error *e)
{
  void *dest = arena_malloc (alloc, size, 1, e);

  if (dest == NULL) {
    return NULL;
  }

  memcpy (dest, ptr, size);

  return dest;
}

static err_t
arena_alloc_add_new_chunk (struct arena_alloc *ca, const u32 size, error *e)
{
  DBG_ASSERT (arena_alloc, ca);

  // Check chunk count limit
  if (ca->settings.max_chunks > 0 && ca->num_chunks >= ca->settings.max_chunks) {
    return error_causef (
        e,
        ERR_NOMEM,
        "chunk limit reached (%u/%u chunks)",
        ca->num_chunks,
        ca->settings.max_chunks
    );
  }

  // Verify size constraints (internal assertions)
  ASSERT (size >= ca->settings.min_chunk_size);
  ASSERT (ca->settings.max_chunk_size == 0 || size <= ca->settings.max_chunk_size);

  // Check total memory limit
  if ((ca->settings.max_total_size > 0)
      && (ca->total_allocated + size > ca->settings.max_total_size)) {
    return error_causef (
        e,
        ERR_NOMEM,
        "alloc %u bytes would exceed %u "
        "byte limit (%u allocated)",
        size,
        ca->settings.max_total_size,
        ca->total_allocated
    );
  }

  // Create chunk
  struct chunk *new_chunk = chunk_create (size, ca->settings.mem, e);
  if (new_chunk == NULL) {
    return error_trace (e);
  }

  // Add to front of list
  new_chunk->next = ca->head;
  ca->head        = new_chunk;
  ca->num_chunks++;
  ca->total_allocated += size;

  return SUCCESS;
}

void *
arena_malloc (struct arena_alloc *ca, const u32 req, const u32 size, error *e)
{
  latch_lock (&ca->latch);

  DBG_ASSERT (arena_alloc, ca);

  // Check for overflow
  if (req > 0 && size > UINT32_MAX / req) {
    error_causef (e, ERR_NOMEM, "alloc overflow: %u * %u", req, size);
    latch_unlock (&ca->latch);
    return NULL;
  }

  const u32 alloc_size = req * size;

  // Check single allocation limit
  if (ca->settings.max_alloc_size > 0 && alloc_size > ca->settings.max_alloc_size) {
    error_causef (
        e,
        ERR_NOMEM,
        "alloc %u bytes exceeds max %u",
        alloc_size,
        ca->settings.max_alloc_size
    );
    latch_unlock (&ca->latch);
    return NULL;
  }

  // Try current chunk first
  if (ca->head != NULL) {
    void *ptr = lmalloc (&ca->head->alloc, req, size, NULL);
    if (ptr != NULL) {
      ca->total_used += alloc_size;
      latch_unlock (&ca->latch);
      return ptr;
    }
  }

  // Need a new chunk - calculate size
  const u32 new_chunk_size = compute_new_chunk_size (ca, alloc_size);

  // Create new chunk
  if (arena_alloc_add_new_chunk (ca, new_chunk_size, e) != SUCCESS) {
    latch_unlock (&ca->latch);
    return NULL;
  }

  // Allocate from new chunk
  void *ptr = lmalloc (&ca->head->alloc, req, size, e);
  if (ptr != NULL) {
    ca->total_used += alloc_size;
  }

  latch_unlock (&ca->latch);

  return ptr;
}

void
builder_init (struct builder *b, struct arena_alloc *alloc)
{
  b->persistent = alloc;
  arena_alloc_create_default (&b->temp);
}

void *
builder_malloc_temp (struct builder *b, u32 nelem, u32 size, error *e)
{
  return arena_malloc (&b->temp, nelem, size, e);
}

void *
builder_malloc_persist (struct builder *b, u32 nelem, u32 size, error *e)
{
  return arena_malloc (b->persistent, nelem, size, e);
}

void
builder_free (struct builder *b)
{
  arena_alloc_free_all (&b->temp);
}

#ifndef NDEBUG

/******************************************************************************
 * SECTION: Tests
 ******************************************************************************/

static struct arena_alloc_settings
test_settings (struct i_mem mem)
{
  return (struct arena_alloc_settings){
      .max_alloc_size    = 0,
      .max_total_size    = 0,
      .target_chunk_mult = 10,
      .min_chunk_size    = 10,
      .max_chunk_size    = 0,
      .max_chunks        = 0,
      .mem               = mem,
  };
}

TEST (arena_malloc_refuses_past_its_limits)
{
  TEST_CASE ("req * size overflows u32")
  {
    struct arena_alloc          a;
    struct arena_alloc_settings st = test_settings (ctx->mem);
    arena_alloc_create (&a, st);

    test_assert_ptr_equal (arena_malloc (&a, 0x10000, 0x10000, &ctx->e), NULL);
    test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);
    error_reset (&ctx->e);

    test_assert (arena_malloc (&a, 1, 8, &ctx->e) != NULL);
    arena_alloc_free_all (&a);
  }

  TEST_CASE ("one allocation larger than max_alloc_size")
  {
    struct arena_alloc          a;
    struct arena_alloc_settings st = test_settings (ctx->mem);
    st.max_alloc_size              = 64;
    arena_alloc_create (&a, st);

    test_assert (arena_malloc (&a, 64, 1, &ctx->e) != NULL); // exactly at the limit
    test_assert_ptr_equal (arena_malloc (&a, 65, 1, &ctx->e), NULL);
    test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);
    error_reset (&ctx->e);

    arena_alloc_free_all (&a);
  }

  TEST_CASE ("an allocation that exactly fills a chunk is accepted")
  {
    struct arena_alloc          a;
    struct arena_alloc_settings st = test_settings (ctx->mem);
    st.target_chunk_mult           = 1;
    st.min_chunk_size              = 1;
    arena_alloc_create (&a, st);

    void *exact = arena_malloc (&a, 32, 1, &ctx->e);
    test_assert (exact != NULL);
    test_assert_int_equal (a.total_allocated, 32);

    // And the chunk really is full now, so the next byte needs a second chunk.
    test_assert (arena_malloc (&a, 1, 1, &ctx->e) != NULL);
    test_assert_int_equal (a.num_chunks, 2);

    arena_alloc_free_all (&a);
  }

  TEST_CASE ("a new chunk would push past max_total_size")
  {
    struct arena_alloc          a;
    struct arena_alloc_settings st = test_settings (ctx->mem);
    st.target_chunk_mult           = 1;
    st.min_chunk_size              = 32;
    st.max_chunk_size              = 32;
    st.max_total_size              = 32;
    arena_alloc_create (&a, st);

    test_assert (arena_malloc (&a, 8, 1, &ctx->e) != NULL);
    test_assert_int_equal (a.total_allocated, 32);

    test_assert_ptr_equal (arena_malloc (&a, 32, 1, &ctx->e), NULL);
    test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);
    error_reset (&ctx->e);

    test_assert_int_equal (a.num_chunks, 1);
    test_assert_int_equal (a.total_allocated, 32);
    arena_alloc_free_all (&a);
  }

  TEST_CASE ("the backing allocator fails while creating a chunk")
  {
    struct arena_alloc          a;
    struct arena_alloc_settings st = test_settings (ctx->faulty_mem);
    arena_alloc_create (&a, st);

    faulty_mem_reset (ctx->faulty_mem);
    faulty_mem_fail_after (ctx->faulty_mem, 0);

    test_assert_ptr_equal (arena_malloc (&a, 1, 8, &ctx->e), NULL);
    test_assert (ctx->e.cause_code < 0);
    error_reset (&ctx->e);
    test_assert_int_equal (a.num_chunks, 0);

    // The budget is spent, so the retry has to succeed.
    test_assert (arena_malloc (&a, 1, 8, &ctx->e) != NULL);
    test_assert_int_equal (a.num_chunks, 1);

    arena_alloc_free_all (&a);
    faulty_mem_reset (ctx->faulty_mem);
  }
}

TEST (arena_malloc_respects_max_chunks)
{
  struct arena_alloc          a;
  struct arena_alloc_settings st = test_settings (ctx->mem);
  st.target_chunk_mult           = 1;
  st.min_chunk_size              = 16;
  st.max_chunk_size              = 16;
  st.max_chunks                  = 2;
  arena_alloc_create (&a, st);

  test_assert (arena_malloc (&a, 15, 1, &ctx->e) != NULL);
  test_assert (arena_malloc (&a, 15, 1, &ctx->e) != NULL);
  test_assert_int_equal (a.num_chunks, 2);

  test_assert_ptr_equal (arena_malloc (&a, 15, 1, &ctx->e), NULL);
  test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);
  error_reset (&ctx->e);

  test_assert_int_equal (a.num_chunks, 2);
  arena_alloc_free_all (&a);
}

TEST (compute_new_chunk_size_clamps)
{
  struct arena_alloc          a;
  struct arena_alloc_settings st = test_settings (ctx->mem);

  TEST_CASE ("clamped up to min_chunk_size")
  {
    st.target_chunk_mult = 1;
    st.min_chunk_size    = 4096;
    arena_alloc_create (&a, st);
    test_assert_int_equal (compute_new_chunk_size (&a, 8), 4096);
    arena_alloc_free_all (&a);
  }

  TEST_CASE ("clamped down to max_chunk_size")
  {
    st.target_chunk_mult = 10;
    st.min_chunk_size    = 8;
    st.max_chunk_size    = 64;
    arena_alloc_create (&a, st);
    test_assert_int_equal (compute_new_chunk_size (&a, 32), 64);
    arena_alloc_free_all (&a);
  }

  TEST_CASE ("grown back up to fit the allocation")
  {
    // target_chunk_mult is a float, so for an alloc_size past 2^24 the
    // product rounds *down* and the target no longer fits the request.
    st.target_chunk_mult = 1;
    st.min_chunk_size    = 8;
    st.max_chunk_size    = 0;
    arena_alloc_create (&a, st);

    const u32 big = (1u << 24) + 1;
    test_assert ((u32)(big * st.target_chunk_mult) < big);
    test_assert_int_equal (compute_new_chunk_size (&a, big), big);
    arena_alloc_free_all (&a);
  }

  TEST_CASE ("random settings never undercut the request or the minimum")
  {
    for (u32 i = 0; i < 500; ++i) {
      st.target_chunk_mult = 1.0f + (float)randu32r (0, 20);
      st.min_chunk_size    = randu32r (1, 4096);
      st.max_chunk_size    = randu32r (0, 1) ? 0 : randu32r (st.min_chunk_size, 8192);
      arena_alloc_create (&a, st);

      const u32 alloc_size = randu32r (1, 10000);
      const u32 got        = compute_new_chunk_size (&a, alloc_size);

      test_assert (got >= alloc_size);
      test_assert (got >= st.min_chunk_size || got == alloc_size);
      arena_alloc_free_all (&a);
    }
  }
}

TEST (arena_alloc_copy)
{
  TEST_CASE ("copies the bytes")
  {
    struct arena_alloc a;
    arena_alloc_create_default_with (&a, ctx->mem);

    const char *src = "the quick brown fox";
    char       *dst = arena_alloc_copy (&a, src, (u32)strlen (src) + 1, &ctx->e);
    test_assert (dst != NULL);
    test_assert (dst != src);
    test_assert_int_equal (strcmp (dst, src), 0);

    arena_alloc_free_all (&a);
  }

  TEST_CASE ("returns NULL when the arena cannot serve the request")
  {
    struct arena_alloc          a;
    struct arena_alloc_settings st = test_settings (ctx->mem);
    st.max_alloc_size              = 4;
    arena_alloc_create (&a, st);

    u8 src[16] = {0};
    test_assert_ptr_equal (arena_alloc_copy (&a, src, sizeof (src), &ctx->e), NULL);
    test_assert_int_equal (ctx->e.cause_code, ERR_NOMEM);
    error_reset (&ctx->e);

    arena_alloc_free_all (&a);
  }
}

TEST (arena_alloc_free_all_resets_accounting)
{
  struct arena_alloc a;
  arena_alloc_create_default_with (&a, ctx->mem);

  for (u32 i = 0; i < 50; ++i) {
    test_assert (arena_malloc (&a, randu32r (1, 200), 1, &ctx->e) != NULL);
  }
  test_assert (a.num_chunks > 0);
  test_assert (a.total_used <= a.total_allocated);

  arena_alloc_free_all (&a);

  test_assert_ptr_equal (a.head, NULL);
  test_assert_int_equal (a.num_chunks, 0);
  test_assert_int_equal (a.total_allocated, 0);
  test_assert_int_equal (a.total_used, 0);

  test_assert (arena_malloc (&a, 1, 8, &ctx->e) != NULL);
  arena_alloc_free_all (&a);
}

#endif // NDEBUG
