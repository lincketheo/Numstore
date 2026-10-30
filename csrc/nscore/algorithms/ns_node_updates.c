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

#include "nscore/algorithms/ns_node_updates.h"

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_numerics.h"
#include "core/os/ns_malloc.h"

#ifndef NDEBUG
#  include "core/ns_testing.h"
#  include "nscore/page/ns_page.h"
#endif

#include <string.h>

#define MAX_INNER_NODES_PER_NUPD 6
#define NUPD_LENGTH              (MAX_INNER_NODES_PER_NUPD * IN_MAX_KEYS)

struct in_pair_slab
{
  struct in_pair_slab *next;
  struct in_pair       data[NUPD_LENGTH];
};

struct node_updates
{
  struct i_mem        mem;
  struct slab_alloc  *alloc;

  struct in_pair      pivot;

  struct in_pair_slab right;
  struct in_pair_slab left;

  struct in_pair     *prev;

  // Length
  u32                 rlen;
  u32                 llen;

  // Observed
  u32                 robs;
  u32                 lobs;

  // Consumed
  u32                 rcons;
  u32                 lcons;
};

// smartfiles
DEFINE_DBG_ASSERT (struct node_updates, node_updates, n, {
  ASSERT (n);
  ASSERT (!in_pair_is_empty (n->pivot));

  // This is the main feature
  // You can't consume a node unless you've
  // observed it
  ASSERT (n->rcons <= n->robs);
  ASSERT (n->lcons <= n->lobs);
})

////////////////////////////////////////////////////////////
// Slab navigation helpers
////////////////////////////////////////////////////////////

static struct in_pair_slab *
slab_at (struct in_pair_slab *head, const u32 slab_idx)
{
  struct in_pair_slab *cur = head;
  for (u32 i = 0; i < slab_idx && cur != NULL; ++i) {
    cur = cur->next;
  }
  return cur;
}

static struct in_pair *
nupd_get_right (struct node_updates *s, const u32 idx)
{
  const u32            slab_idx  = idx / NUPD_LENGTH;
  const u32            local_idx = idx % NUPD_LENGTH;
  struct in_pair_slab *slab      = slab_at (&s->right, slab_idx);
  ASSERT (slab != NULL);
  return &slab->data[local_idx];
}

static struct in_pair *
nupd_get_left (struct node_updates *s, const u32 idx)
{
  const u32            slab_idx  = idx / NUPD_LENGTH;
  const u32            local_idx = idx % NUPD_LENGTH;
  struct in_pair_slab *slab      = slab_at (&s->left, slab_idx);
  ASSERT (slab != NULL);
  return &slab->data[local_idx];
}

static struct in_pair *
nupd_push_right (struct node_updates *s, const pgno pg, const b_size size, error *e)
{
  const u32            slab_idx  = s->rlen / NUPD_LENGTH;
  const u32            local_idx = s->rlen % NUPD_LENGTH;

  struct in_pair_slab *slab      = &s->right;
  for (u32 i = 0; i < slab_idx; ++i) {
    if (slab->next == NULL) {
      slab->next = s->mem.table->malloc (s->mem.self, 1, sizeof *slab->next, e);
      if (slab->next == NULL) {
        return NULL;
      }
      memset (slab->next, 0, sizeof *slab->next);
    }
    slab = slab->next;
  }

  slab->data[local_idx] = in_pair_from (pg, size);
  s->rlen++;
  return &slab->data[local_idx];
}

static struct in_pair *
nupd_push_left (struct node_updates *s, const pgno pg, const b_size size, error *e)
{
  const u32            slab_idx  = s->llen / NUPD_LENGTH;
  const u32            local_idx = s->llen % NUPD_LENGTH;

  struct in_pair_slab *slab      = &s->left;
  for (u32 i = 0; i < slab_idx; ++i) {
    if (slab->next == NULL) {
      slab->next = s->mem.table->malloc (s->mem.self, 1, sizeof *slab->next, e);
      if (slab->next == NULL) {
        return NULL;
      }
      memset (slab->next, 0, sizeof *slab->next);
    }
    slab = slab->next;
  }

  slab->data[local_idx] = in_pair_from (pg, size);
  s->llen++;
  return &slab->data[local_idx];
}

static void
slab_free_chain (const struct in_pair_slab *head, struct i_mem mem)
{
  // head is embedded, only free ->next chain
  struct in_pair_slab *cur = head->next;
  while (cur != NULL) {
    struct in_pair_slab *next = cur->next;
    mem.table->free (mem.self, cur);
    cur = next;
  }
}

struct node_updates *
nupd_init (const pgno pg, const b_size size, struct i_mem mem, error *e)
{
  struct node_updates *ret = mem.table->calloc (mem.self, 1, sizeof *ret, e);
  if (ret == NULL) {
    return NULL;
  }

  ret->mem = mem;
  nupd_reset (ret, pg, size);

  return ret;
}

struct node_updates *
nupd_create_from (
    struct in_pair *left,
    u32             llen,
    struct in_pair  pivot,
    struct in_pair *right,
    u32             rlen,
    struct i_mem    mem,
    error          *e
)
{
  struct node_updates *ret = nupd_init (pivot.pg, pivot.key, mem, e);
  if (ret == NULL) {
    return ret;
  }

  for (u32 i = 0; i < llen; ++i) {
    if (nupd_push_left (ret, left[i].pg, left[i].key, e) == NULL) {
      goto failed;
    }
  }

  for (u32 i = 0; i < rlen; ++i) {
    if (nupd_push_right (ret, right[i].pg, right[i].key, e) == NULL) {
      goto failed;
    }
  }

  return ret;

failed:
  nupd_free (ret);
  error_trace (e);
  return NULL;
}

struct node_updates *
nupd_random_from (
    pgno        *left,
    u32          llen,
    pgno         pivot,
    pgno        *right,
    u32          rlen,
    struct i_mem mem,
    error       *e
)
{
  struct node_updates *ret = nupd_init (pivot, randu64r (1, 1000000), mem, e);
  if (ret == NULL) {
    return ret;
  }

  for (u32 i = 0; i < llen; ++i) {
    if (nupd_push_left (ret, left[i], randu64r (1, 1000000), e) == NULL) {
      goto failed;
    }
  }

  for (u32 i = 0; i < rlen; ++i) {
    if (nupd_push_right (ret, right[i], randu64r (1, 1000000), e) == NULL) {
      goto failed;
    }
  }

  return ret;

failed:
  nupd_free (ret);
  return NULL;
}

void
nupd_reset (struct node_updates *ret, const pgno pg, const b_size size)
{
  struct i_mem mem = ret->mem;

  // Free any allocated slabs first
  slab_free_chain (&ret->right, mem);
  slab_free_chain (&ret->left, mem);

  memset (ret, 0, sizeof *ret);
  ret->mem   = mem;
  ret->pivot = in_pair_from (pg, size);

  DBG_ASSERT (node_updates, ret);

  ret->prev = &ret->pivot;
}

void
nupd_free (struct node_updates *n)
{
  if (n == NULL) {
    return;
  }
  struct i_mem mem = n->mem;
  slab_free_chain (&n->right, mem);
  slab_free_chain (&n->left, mem);
  mem.table->free (mem.self, n);
}

pgno
nupd_pivot_pg (const struct node_updates *n)
{
  return n->pivot.pg;
}

#ifndef NDEBUG
TEST (nupd_init)
{
  TEST_CASE ("Initialize with page and size")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    test_assert_equal (n->pivot.pg, 100);
    test_assert_equal (n->pivot.key, 512);
    test_assert_equal (n->llen, 0);
    test_assert_equal (n->rlen, 0);
    test_assert_equal (n->lobs, 0);
    test_assert_equal (n->robs, 0);
    test_assert_equal (n->lcons, 0);
    test_assert_equal (n->rcons, 0);

    nupd_free (n);
  }

  TEST_CASE ("Initialize with zero values")
  {
    struct node_updates *n = nupd_init (0, 0, ctx->mem, &ctx->e);

    test_assert_equal (n->pivot.pg, 0);
    test_assert_equal (n->pivot.key, 0);

    nupd_free (n);
  }

  TEST_CASE ("Initialize with large values")
  {
    struct node_updates *n = nupd_init (999999, 65536, ctx->mem, &ctx->e);

    test_assert_equal (n->pivot.pg, 999999);
    test_assert_equal (n->pivot.key, 65536);

    nupd_free (n);
  }
}

#endif

#ifndef NDEBUG
// create_from is push-based, not append-based: it lays the entries down as
// given, so a left entry whose pg equals the pivot's is a real entry and not
// an update to the pivot (which is what append_left would have made it).
TEST (nupd_create_from)
{
  TEST_CASE ("Copies left, pivot and right")
  {
    struct in_pair       left[]  = {{.pg = 10, .key = 100}, {.pg = 11, .key = 101}};
    const struct in_pair pivot   = {.pg = 20, .key = 200};
    struct in_pair       right[] = {
        {.pg = 30, .key = 300},
        {.pg = 31, .key = 301},
        {.pg = 32, .key = 302}
    };

    struct node_updates *n = nupd_create_from (left, 2, pivot, right, 3, ctx->mem, &ctx->e);
    test_assert (n != NULL);

    test_assert_equal (n->pivot.pg, 20);
    test_assert_equal (n->pivot.key, 200);

    test_assert_equal (n->llen, 2);
    for (u32 i = 0; i < 2; ++i) {
      test_assert_equal (nupd_get_left (n, i)->pg, left[i].pg);
      test_assert_equal (nupd_get_left (n, i)->key, left[i].key);
    }

    test_assert_equal (n->rlen, 3);
    for (u32 i = 0; i < 3; ++i) {
      test_assert_equal (nupd_get_right (n, i)->pg, right[i].pg);
      test_assert_equal (nupd_get_right (n, i)->key, right[i].key);
    }

    // Nothing has been observed or consumed yet
    test_assert_equal (n->lobs, 0);
    test_assert_equal (n->robs, 0);
    test_assert_equal (n->lcons, 0);
    test_assert_equal (n->rcons, 0);

    nupd_free (n);
  }

  TEST_CASE ("Empty sides leave only the pivot")
  {
    const struct in_pair pivot = {.pg = 20, .key = 200};

    struct node_updates *n     = nupd_create_from (NULL, 0, pivot, NULL, 0, ctx->mem, &ctx->e);
    test_assert (n != NULL);

    test_assert_equal (n->pivot.pg, 20);
    test_assert_equal (n->pivot.key, 200);
    test_assert_equal (n->llen, 0);
    test_assert_equal (n->rlen, 0);

    nupd_free (n);
  }

  TEST_CASE ("A side longer than one slab spills into the chain")
  {
    const u32       llen = NUPD_LENGTH + 3;
    struct in_pair *left = ctx->mem.table->malloc (ctx->mem.self, llen, sizeof *left, &ctx->e);
    test_assert (left != NULL);
    for (u32 i = 0; i < llen; ++i) {
      left[i] = in_pair_from (1000 + i, 2000 + i);
    }

    const struct in_pair pivot = {.pg = 20, .key = 200};

    struct node_updates *n     = nupd_create_from (left, llen, pivot, NULL, 0, ctx->mem, &ctx->e);
    test_assert (n != NULL);

    test_assert_equal (n->llen, llen);
    test_assert_equal (nupd_get_left (n, 0)->pg, 1000);
    test_assert_equal (nupd_get_left (n, NUPD_LENGTH)->pg, 1000 + NUPD_LENGTH);
    test_assert_equal (nupd_get_left (n, llen - 1)->pg, 1000 + llen - 1);

    nupd_free (n);
    ctx->mem.table->free (ctx->mem.self, left);
  }

  TEST_CASE ("A left entry on the pivot page stays a separate entry")
  {
    struct in_pair       left[] = {{.pg = 20, .key = 999}};
    const struct in_pair pivot  = {.pg = 20, .key = 200};

    struct node_updates *n      = nupd_create_from (left, 1, pivot, NULL, 0, ctx->mem, &ctx->e);
    test_assert (n != NULL);

    test_assert_equal (n->pivot.key, 200); // Not folded into the pivot
    test_assert_equal (n->llen, 1);
    test_assert_equal (nupd_get_left (n, 0)->key, 999);

    nupd_free (n);
  }

  TEST_CASE ("A failed node allocation returns NULL")
  {
    struct in_pair       left[] = {{.pg = 10, .key = 100}};
    const struct in_pair pivot  = {.pg = 20, .key = 200};

    faulty_mem_reset (ctx->faulty_mem);
    faulty_mem_fail_after (ctx->faulty_mem, 0);

    test_assert_ptr_equal (
        nupd_create_from (left, 1, pivot, NULL, 0, ctx->faulty_mem, &ctx->e),
        NULL
    );
    error_reset (&ctx->e);

    faulty_mem_reset (ctx->faulty_mem);
  }

  TEST_CASE ("A failed slab allocation frees the partial result")
  {
    // The head slab is embedded, so only a side that spills needs memory: the
    // node's own calloc goes through and the first slab malloc does not.
    const u32       llen = NUPD_LENGTH + 1;
    struct in_pair *left = ctx->mem.table->malloc (ctx->mem.self, llen, sizeof *left, &ctx->e);
    test_assert (left != NULL);
    for (u32 i = 0; i < llen; ++i) {
      left[i] = in_pair_from (1000 + i, 2000 + i);
    }

    const struct in_pair pivot = {.pg = 20, .key = 200};

    faulty_mem_reset (ctx->faulty_mem);
    faulty_mem_fail_after (ctx->faulty_mem, 1);

    test_assert_ptr_equal (
        nupd_create_from (left, llen, pivot, NULL, 0, ctx->faulty_mem, &ctx->e),
        NULL
    );
    error_reset (&ctx->e);

    faulty_mem_reset (ctx->faulty_mem);
    ctx->mem.table->free (ctx->mem.self, left);
  }
}
#endif

static struct in_pair *
nupd_append_right (struct node_updates *s, const pgno pg, const b_size size, error *e)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->robs == 0);
  ASSERT (s->lobs == 0);
  ASSERT (s->rcons == 0);
  ASSERT (s->lcons == 0);

  struct in_pair *ret;

  if (s->rlen == 0 && pg == s->pivot.pg) {
    ret          = &s->pivot;
    s->pivot.key = size;
  } else {
    ret = nupd_push_right (s, pg, size, e);
  }

  return ret;
}

#ifndef NDEBUG
TEST (nupd_append_right)
{
  TEST_CASE ("Append single right entry")
  {
    struct node_updates  *n   = nupd_init (100, 512, ctx->mem, &ctx->e);

    const struct in_pair *ret = nupd_append_right (n, 200, 1024, &ctx->e);

    test_assert_equal (n->rlen, 1);
    test_assert_equal (nupd_get_right (n, 0)->pg, 200);
    test_assert_equal (nupd_get_right (n, 0)->key, 1024);
    test_assert_equal (ret, nupd_get_right (n, 0));

    nupd_free (n);
  }

  TEST_CASE ("Append to pivot when page matches")
  {
    struct node_updates  *n   = nupd_init (100, 512, ctx->mem, &ctx->e);
    const struct in_pair *ret = nupd_append_right (n, 100, 1024, &ctx->e);

    test_assert_equal (n->rlen, 0);
    test_assert_equal (n->pivot.pg, 100);
    test_assert_equal (n->pivot.key, 1024);
    test_assert_equal (ret, &n->pivot);

    nupd_free (n);
  }

  TEST_CASE ("Append multiple entries")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_append_right (n, 200, 1024, &ctx->e);
    nupd_append_right (n, 300, 2048, &ctx->e);
    nupd_append_right (n, 400, 4096, &ctx->e);

    test_assert_equal (n->rlen, 3);
    test_assert_equal (nupd_get_right (n, 0)->pg, 200);
    test_assert_equal (nupd_get_right (n, 1)->pg, 300);
    test_assert_equal (nupd_get_right (n, 2)->pg, 400);
    test_assert_equal (nupd_get_right (n, 0)->key, 1024);
    test_assert_equal (nupd_get_right (n, 1)->key, 2048);
    test_assert_equal (nupd_get_right (n, 2)->key, 4096);

    nupd_free (n);
  }

  TEST_CASE ("Append after pivot update")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_append_right (n, 100, 1024, &ctx->e); // Updates pivot
    nupd_append_right (n, 200, 2048, &ctx->e); // Should add to array

    test_assert_equal (n->rlen, 1);
    test_assert_equal (n->pivot.key, 1024);
    test_assert_equal (nupd_get_right (n, 0)->pg, 200);

    nupd_free (n);
  }

  TEST_CASE ("Append beyond single slab")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    for (u32 i = 0; i < NUPD_LENGTH + 5; ++i) {
      nupd_append_right (n, 200 + i, 1000 + i, &ctx->e);
    }

    test_assert_equal (n->rlen, NUPD_LENGTH + 5);
    test_assert_equal (nupd_get_right (n, 0)->pg, 200);
    test_assert_equal (nupd_get_right (n, NUPD_LENGTH)->pg, 200 + NUPD_LENGTH);
    test_assert_equal (nupd_get_right (n, NUPD_LENGTH + 4)->pg, 200 + NUPD_LENGTH + 4);

    nupd_free (n);
  }
}
#endif

static struct in_pair *
nupd_append_left (struct node_updates *s, const pgno pg, const b_size size, error *e)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->robs == 0);
  ASSERT (s->lobs == 0);
  ASSERT (s->rcons == 0);
  ASSERT (s->lcons == 0);

  struct in_pair *ret;

  if (s->llen == 0 && pg == s->pivot.pg) {
    ret          = &s->pivot;
    s->pivot.key = size;
  } else {
    ret = nupd_push_left (s, pg, size, e);
  }

  return ret;
}

#ifndef NDEBUG
TEST (nupd_append_left)
{
  TEST_CASE ("Append single left entry")
  {
    struct node_updates  *n   = nupd_init (100, 512, ctx->mem, &ctx->e);

    const struct in_pair *ret = nupd_append_left (n, 50, 256, &ctx->e);

    test_assert_equal (n->llen, 1);
    test_assert_equal (nupd_get_left (n, 0)->pg, 50);
    test_assert_equal (nupd_get_left (n, 0)->key, 256);
    test_assert_equal (ret, nupd_get_left (n, 0));

    nupd_free (n);
  }

  TEST_CASE ("Append to pivot when page matches")
  {
    struct node_updates  *n   = nupd_init (100, 512, ctx->mem, &ctx->e);

    const struct in_pair *ret = nupd_append_left (n, 100, 768, &ctx->e);

    test_assert_equal (n->llen, 0);
    test_assert_equal (n->pivot.pg, 100);
    test_assert_equal (n->pivot.key, 768);
    test_assert_equal (ret, &n->pivot);

    nupd_free (n);
  }

  TEST_CASE ("Append multiple entries")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_append_left (n, 90, 256, &ctx->e);
    nupd_append_left (n, 80, 128, &ctx->e);
    nupd_append_left (n, 70, 64, &ctx->e);

    test_assert_equal (n->llen, 3);
    test_assert_equal (nupd_get_left (n, 0)->pg, 90);
    test_assert_equal (nupd_get_left (n, 1)->pg, 80);
    test_assert_equal (nupd_get_left (n, 2)->pg, 70);

    nupd_free (n);
  }

  TEST_CASE ("Append after pivot update")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_append_left (n, 100, 256, &ctx->e); // Updates pivot
    nupd_append_left (n, 50, 128, &ctx->e);  // Should add to array

    test_assert_equal (n->llen, 1);
    test_assert_equal (n->pivot.key, 256);
    test_assert_equal (nupd_get_left (n, 0)->pg, 50);

    nupd_free (n);
  }

  TEST_CASE ("Append beyond single slab")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    for (u32 i = 0; i < NUPD_LENGTH + 5; ++i) {
      nupd_append_left (n, 50 + i, 100 + i, &ctx->e);
    }

    test_assert_equal (n->llen, NUPD_LENGTH + 5);
    test_assert_equal (nupd_get_left (n, NUPD_LENGTH)->pg, 50 + NUPD_LENGTH);

    nupd_free (n);
  }
}
#endif

err_t
nupd_commit_1st_right (struct node_updates *s, const pgno pg, const b_size size, error *e)
{
  if (s->prev != NULL) {
    ASSERT (s->prev->pg == pg);
    s->prev->key = size;
    s->prev      = NULL;
  } else {
    if (nupd_append_right (s, pg, size, e) == NULL) {
      return error_trace (e);
    }
  }
  return SUCCESS;
}

err_t
nupd_commit_1st_left (struct node_updates *s, const pgno pg, const b_size size, error *e)
{
  if (s->prev != NULL) {
    ASSERT (s->prev->pg == pg);
    s->prev->key = size;
    s->prev      = NULL;
  } else {
    if (nupd_append_left (s, pg, size, e) == NULL) {
      return error_trace (e);
    }
  }
  return SUCCESS;
}

err_t
nupd_append_2nd_right (
    struct node_updates *s,
    const pgno           pg1,
    const b_size         size1,
    const pgno           pg2,
    const b_size         size2,
    error               *e
)
{
  if (s->prev != NULL) {
    ASSERT (s->prev->pg == pg1);
  } else {
    s->prev = nupd_append_right (s, pg1, size1, e);
    if (s->prev == NULL) {
      return error_trace (e);
    }
  }
  if (nupd_append_right (s, pg2, size2, e) == NULL) {
    return error_trace (e);
  }
  return SUCCESS;
}

err_t
nupd_append_2nd_left (
    struct node_updates *s,
    const pgno           pg1,
    const b_size         size1,
    const pgno           pg2,
    const b_size         size2,
    error               *e
)
{
  if (s->prev != NULL) {
    ASSERT (s->prev->pg == pg1);
  } else {
    s->prev = nupd_append_left (s, pg1, size1, e);
    if (s->prev == NULL) {
      return error_trace (e);
    }
  }
  if (nupd_append_left (s, pg2, size2, e) == NULL) {
    return error_trace (e);
  }
  return SUCCESS;
}

#ifndef NDEBUG
TEST (nupd_append_2nd_left)
{
  TEST_CASE ("With no pending prev, both entries are appended")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    n->prev                = NULL;

    test_assert_int_equal (nupd_append_2nd_left (n, 50, 256, 40, 128, &ctx->e), SUCCESS);

    test_assert_equal (n->llen, 2);
    test_assert_equal (nupd_get_left (n, 0)->pg, 50);
    test_assert_equal (nupd_get_left (n, 0)->key, 256);
    test_assert_equal (nupd_get_left (n, 1)->pg, 40);
    test_assert_equal (nupd_get_left (n, 1)->key, 128);
    test_assert_equal (n->rlen, 0);

    // prev is left pointing at the first of the two
    test_assert_ptr_equal (n->prev, nupd_get_left (n, 0));

    nupd_free (n);
  }

  TEST_CASE ("A pending prev is reused, so only the second is appended")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    // nupd_init leaves prev pointing at the pivot
    test_assert_ptr_equal (n->prev, &n->pivot);

    test_assert_int_equal (nupd_append_2nd_left (n, 100, 256, 40, 128, &ctx->e), SUCCESS);

    // pg1 matched the pending prev, so it did not become a new entry - and its
    // size1 is ignored, the pending entry already carries its own key
    test_assert_equal (n->pivot.key, 512);
    test_assert_equal (n->llen, 1);
    test_assert_equal (nupd_get_left (n, 0)->pg, 40);
    test_assert_equal (nupd_get_left (n, 0)->key, 128);

    nupd_free (n);
  }

  TEST_CASE ("pg1 on the pivot page with nothing stored updates the pivot")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    n->prev                = NULL;

    test_assert_int_equal (nupd_append_2nd_left (n, 100, 256, 40, 128, &ctx->e), SUCCESS);

    test_assert_equal (n->pivot.key, 256); // append_left folded pg1 into the pivot
    test_assert_ptr_equal (n->prev, &n->pivot);
    test_assert_equal (n->llen, 1);
    test_assert_equal (nupd_get_left (n, 0)->pg, 40);

    nupd_free (n);
  }

  TEST_CASE ("Repeated calls keep stacking away from the pivot")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    n->prev                = NULL;

    test_assert_int_equal (nupd_append_2nd_left (n, 50, 256, 40, 128, &ctx->e), SUCCESS);
    n->prev = NULL;
    test_assert_int_equal (nupd_append_2nd_left (n, 30, 64, 20, 32, &ctx->e), SUCCESS);

    test_assert_equal (n->llen, 4);
    test_assert_equal (nupd_get_left (n, 2)->pg, 30);
    test_assert_equal (nupd_get_left (n, 3)->pg, 20);

    nupd_free (n);
  }

  // The head slab is embedded, so only a push that spills past NUPD_LENGTH
  // allocates - which is where an injected fault has to land.
  TEST_CASE ("A failed allocation on the first entry is surfaced")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->faulty_mem, &ctx->e);
    test_assert (n != NULL);

    for (u32 i = 0; i < NUPD_LENGTH; ++i) {
      test_assert (nupd_append_left (n, 1000 + i, 2000 + i, &ctx->e) != NULL);
    }
    test_assert_equal (n->llen, NUPD_LENGTH);
    n->prev = NULL;

    faulty_mem_reset (ctx->faulty_mem);
    faulty_mem_fail_after (ctx->faulty_mem, 0);

    test_assert (nupd_append_2nd_left (n, 50, 256, 40, 128, &ctx->e) != SUCCESS);
    error_reset (&ctx->e);

    faulty_mem_reset (ctx->faulty_mem);
    nupd_free (n);
  }

  TEST_CASE ("A failed allocation on the second entry is surfaced")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->faulty_mem, &ctx->e);
    test_assert (n != NULL);

    // One short of the slab boundary: the first push fits, the second spills
    for (u32 i = 0; i < NUPD_LENGTH - 1; ++i) {
      test_assert (nupd_append_left (n, 1000 + i, 2000 + i, &ctx->e) != NULL);
    }
    test_assert_equal (n->llen, NUPD_LENGTH - 1);
    n->prev = NULL;

    faulty_mem_reset (ctx->faulty_mem);
    faulty_mem_fail_after (ctx->faulty_mem, 0);

    test_assert (nupd_append_2nd_left (n, 50, 256, 40, 128, &ctx->e) != SUCCESS);
    error_reset (&ctx->e);

    // The first of the two did land
    test_assert_equal (n->llen, NUPD_LENGTH);
    test_assert_equal (nupd_get_left (n, NUPD_LENGTH - 1)->pg, 50);

    faulty_mem_reset (ctx->faulty_mem);
    nupd_free (n);
  }
}
#endif

err_t
nupd_append_tip_right (struct node_updates *s, const struct three_in_pair output, error *e)
{
  const err_t rc = nupd_commit_1st_right (s, output.cur.pg, output.cur.key, e);
  if (rc != SUCCESS) {
    return rc;
  }

  if (!in_pair_is_empty (output.prev)) {
    // Search right array backwards
    for (u32 i = s->rlen; i > 0; --i) {
      struct in_pair *p = nupd_get_right (s, i - 1);
      if (p->pg == output.prev.pg) {
        p->key = output.prev.key;
        goto regular;
      }
    }

    // Check pivot
    if (s->pivot.pg == output.prev.pg) {
      s->pivot.key = output.prev.key;
      goto regular;
    }

    // Search left array
    for (u32 i = 0; i < s->llen; ++i) {
      struct in_pair *p = nupd_get_left (s, i);
      if (p->pg == output.prev.pg) {
        p->key = output.prev.key;
        goto regular;
      }
    }

    // Not found, append to left
    if (nupd_append_left (s, output.prev.pg, output.prev.key, e) == NULL) {
      return error_trace (e);
    }
  }

regular:

  if (!in_pair_is_empty (output.next)) {
    if (nupd_append_right (s, output.next.pg, output.next.key, e) == NULL) {
      return error_trace (e);
    }
  }

  return SUCCESS;
}

#ifndef NDEBUG
TEST (nupd_append_tip_right)
{
  TEST_CASE ("Append with only current")
  {
    struct node_updates       *n   = nupd_init (100, 512, ctx->mem, &ctx->e);

    const struct three_in_pair tip = {
        .prev = in_pair_empty,
        .cur  = {.pg = 200, .key = 1024},
        .next = in_pair_empty,
    };

    nupd_commit_1st_right (n, 100, 512, &ctx->e);
    nupd_append_tip_right (n, tip, &ctx->e);

    test_assert_equal (n->rlen, 1);
    test_assert_equal (nupd_get_right (n, 0)->pg, 200);
    test_assert_equal (nupd_get_right (n, 0)->key, 1024);
    test_assert_equal (n->llen, 0);

    nupd_free (n);
  }

  TEST_CASE ("Append with current and next")
  {
    struct node_updates       *n   = nupd_init (100, 512, ctx->mem, &ctx->e);

    const struct three_in_pair tip = {
        .prev = in_pair_empty,
        .cur  = {.pg = 200, .key = 1024},
        .next = {.pg = 300, .key = 2048}
    };

    nupd_commit_1st_right (n, 100, 512, &ctx->e);
    nupd_append_tip_right (n, tip, &ctx->e);

    test_assert_equal (n->rlen, 2);
    test_assert_equal (nupd_get_right (n, 0)->pg, 200);
    test_assert_equal (nupd_get_right (n, 1)->pg, 300);
    test_assert_equal (n->llen, 0);

    nupd_free (n);
  }

  TEST_CASE ("Append with all three pairs")
  {
    struct node_updates       *n   = nupd_init (100, 512, ctx->mem, &ctx->e);

    const struct three_in_pair tip = {
        .prev = {.pg = 150, .key = 768},
        .cur  = {.pg = 200, .key = 1024},
        .next = {.pg = 300, .key = 2048}
    };

    nupd_commit_1st_right (n, 100, 512, &ctx->e);
    nupd_append_tip_right (n, tip, &ctx->e);

    test_assert_equal (n->rlen, 2);
    test_assert_equal (nupd_get_right (n, 0)->pg, 200);
    test_assert_equal (nupd_get_right (n, 1)->pg, 300);
    test_assert_equal (n->llen, 1);
    test_assert_equal (nupd_get_left (n, 0)->pg, 150);

    nupd_free (n);
  }

  TEST_CASE ("Update existing prev in right array")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_commit_1st_right (n, 100, 512, &ctx->e);
    nupd_commit_1st_right (n, 150, 512, &ctx->e);

    const struct three_in_pair tip = {
        .prev = {.pg = 150, .key = 999},
        .cur  = {.pg = 200, .key = 1024},
        .next = in_pair_empty,
    };

    nupd_append_tip_right (n, tip, &ctx->e);

    test_assert_equal (n->rlen, 2);
    test_assert_equal (nupd_get_right (n, 0)->pg, 150);
    test_assert_equal (nupd_get_right (n, 0)->key, 999);
    test_assert_equal (n->llen, 0);

    nupd_free (n);
  }

  TEST_CASE ("Update pivot as prev")
  {
    struct node_updates       *n   = nupd_init (100, 512, ctx->mem, &ctx->e);

    const struct three_in_pair tip = {
        .prev = {.pg = 100, .key = 999},
        .cur  = {.pg = 200, .key = 1024},
        .next = in_pair_empty,
    };

    nupd_commit_1st_right (n, 100, 512, &ctx->e);
    nupd_append_tip_right (n, tip, &ctx->e);

    test_assert_equal (n->pivot.key, 999);
    test_assert_equal (n->rlen, 1);
    test_assert_equal (n->llen, 0);

    nupd_free (n);
  }

  TEST_CASE ("Append prev when not found")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_commit_1st_right (n, 100, 512, &ctx->e);
    nupd_commit_1st_right (n, 200, 1024, &ctx->e);

    const struct three_in_pair tip = {
        .prev = {.pg = 250, .key = 1536},
        .cur  = {.pg = 300, .key = 2048},
        .next = in_pair_empty,
    };

    nupd_append_tip_right (n, tip, &ctx->e);

    test_assert_equal (n->rlen, 2);
    test_assert_equal (n->llen, 1);
    test_assert_equal (nupd_get_left (n, 0)->pg, 250);

    nupd_free (n);
  }
}
#endif

err_t
nupd_append_tip_left (struct node_updates *s, const struct three_in_pair output, error *e)
{
  const err_t rc = nupd_commit_1st_left (s, output.cur.pg, output.cur.key, e);
  if (rc != SUCCESS) {
    return rc;
  }

  if (!in_pair_is_empty (output.next)) {
    // Search left array backwards
    for (u32 i = s->llen; i > 0; --i) {
      struct in_pair *p = nupd_get_left (s, i - 1);
      if (p->pg == output.next.pg) {
        p->key = output.next.key;
        goto regular;
      }
    }

    // Check pivot
    if (s->pivot.pg == output.next.pg) {
      s->pivot.key = output.next.key;
      goto regular;
    }

    // Search right array
    for (u32 i = 0; i < s->rlen; ++i) {
      struct in_pair *p = nupd_get_right (s, i);
      if (p->pg == output.next.pg) {
        p->key = output.next.key;
        goto regular;
      }
    }

    // Not found, append to right
    if (nupd_append_right (s, output.next.pg, output.next.key, e) == NULL) {
      return error_trace (e);
    }
  }

regular:

  if (!in_pair_is_empty (output.prev)) {
    if (nupd_append_left (s, output.prev.pg, output.prev.key, e) == NULL) {
      return error_trace (e);
    }
  }

  return SUCCESS;
}

#ifndef NDEBUG
TEST (nupd_append_tip_left)
{
  TEST_CASE ("Append with only current")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_commit_1st_left (n, 100, 512, &ctx->e);

    const struct three_in_pair tip = {
        .prev = in_pair_empty,
        .cur  = {.pg = 50, .key = 256},
        .next = in_pair_empty,
    };

    nupd_append_tip_left (n, tip, &ctx->e);

    test_assert_equal (n->llen, 1);
    test_assert_equal (nupd_get_left (n, 0)->pg, 50);
    test_assert_equal (nupd_get_left (n, 0)->key, 256);
    test_assert_equal (n->rlen, 0);

    nupd_free (n);
  }

  TEST_CASE ("Append with current and prev")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_commit_1st_left (n, 100, 512, &ctx->e);

    const struct three_in_pair tip = {
        .prev = {.pg = 25, .key = 128},
        .cur  = {.pg = 50, .key = 256},
        .next = in_pair_empty,
    };

    nupd_append_tip_left (n, tip, &ctx->e);

    test_assert_equal (n->llen, 2);
    test_assert_equal (nupd_get_left (n, 0)->pg, 50);
    test_assert_equal (nupd_get_left (n, 1)->pg, 25);
    test_assert_equal (n->rlen, 0);

    nupd_free (n);
  }

  TEST_CASE ("Append with all three pairs")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_commit_1st_left (n, 100, 512, &ctx->e);

    const struct three_in_pair tip = {
        .next = {.pg = 75, .key = 384},
        .cur  = {.pg = 50, .key = 256},
        .prev = {.pg = 25, .key = 128}
    };

    nupd_append_tip_left (n, tip, &ctx->e);

    test_assert_equal (n->llen, 2);
    test_assert_equal (nupd_get_left (n, 0)->pg, 50);
    test_assert_equal (nupd_get_left (n, 1)->pg, 25);
    test_assert_equal (n->rlen, 1);
    test_assert_equal (nupd_get_right (n, 0)->pg, 75);

    nupd_free (n);
  }

  TEST_CASE ("Update existing next in left array")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_commit_1st_left (n, 100, 512, &ctx->e);
    nupd_commit_1st_left (n, 75, 384, &ctx->e);

    const struct three_in_pair tip = {
        .next = {.pg = 75, .key = 999},
        .cur  = {.pg = 50, .key = 256},
        .prev = in_pair_empty,
    };

    nupd_append_tip_left (n, tip, &ctx->e);

    test_assert_equal (n->llen, 2);
    test_assert_equal (nupd_get_left (n, 0)->pg, 75);
    test_assert_equal (nupd_get_left (n, 0)->key, 999);

    nupd_free (n);
  }

  TEST_CASE ("Update pivot as next")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    nupd_commit_1st_left (n, 100, 512, &ctx->e);

    const struct three_in_pair tip = {
        .next = {.pg = 100, .key = 888},
        .cur  = {.pg = 50, .key = 256},
        .prev = in_pair_empty,
    };

    nupd_append_tip_left (n, tip, &ctx->e);

    test_assert_equal (n->pivot.key, 888);
    test_assert_equal (n->llen, 1);
    test_assert_equal (n->rlen, 0);

    nupd_free (n);
  }
}
#endif

static err_t
nupd_observe_right (struct node_updates *s, const pgno pg, const b_size key, error *e)
{
  DBG_ASSERT (node_updates, s);

  if (s->robs == 0 && s->pivot.pg == pg) {
    return SUCCESS;
  }

  while (s->robs < s->rlen) {
    if (nupd_get_right (s, s->robs)->pg == pg) {
      return SUCCESS;
    }
    s->robs++;
  }

  // Need to add new observed entry
  if (nupd_push_right (s, pg, key, e) == NULL) {
    return error_trace (e);
  }
  s->robs++;

  return SUCCESS;
}

static err_t
nupd_observe_left (struct node_updates *s, const pgno pg, const b_size key, error *e)
{
  DBG_ASSERT (node_updates, s);

  if (s->lobs == 0 && s->pivot.pg == pg) {
    return SUCCESS;
  }

  while (s->lobs < s->llen) {
    if (nupd_get_left (s, s->lobs)->pg == pg) {
      return SUCCESS;
    }
    s->lobs++;
  }

  // Need to add new observed entry
  if (nupd_push_left (s, pg, key, e) == NULL) {
    return error_trace (e);
  }
  s->lobs++;

  return SUCCESS;
}

err_t
nupd_observe_pivot (struct node_updates *s, page_h *pg, const p_size lidx, error *e)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->robs == 0);
  ASSERT (s->lobs == 0);
  ASSERT (s->rcons == 0);
  ASSERT (s->lcons == 0);
  ASSERT (page_h_type (pg) == PG_INNER_NODE);

  if (nupd_observe_right_from (s, pg, lidx, e)) {
    return error_trace (e);
  }
  if ((lidx > 0) && nupd_observe_left_from (s, pg, lidx, e)) {
    return error_trace (e);
  }

  return SUCCESS;
}

err_t
nupd_observe_right_from (struct node_updates *s, const page_h *pg, const p_size lidx, error *e)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->robs <= s->rlen);

  if (pg->mode == PHM_NONE) {
    s->robs = s->rlen;
    return SUCCESS;
  }

  ASSERT (page_h_type (pg) == PG_INNER_NODE);

  for (p_size i = lidx; i < in_get_len (page_h_ro (pg)) && i < IN_MAX_KEYS; ++i) {
    const pgno   p  = in_get_leaf (page_h_ro (pg), i);
    const b_size b  = in_get_key (page_h_ro (pg), i);
    const err_t  rc = nupd_observe_right (s, p, b, e);
    if (rc != SUCCESS) {
      return rc;
    }
  }

  return SUCCESS;
}

err_t
nupd_observe_left_from (struct node_updates *s, const page_h *pg, const p_size lidx, error *e)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->lobs <= s->llen);

  if (pg->mode == PHM_NONE) {
    s->lobs = s->llen;
    return SUCCESS;
  }

  ASSERT (page_h_type (pg) == PG_INNER_NODE);

  for (p_size i = lidx; i > 0; --i) {
    const pgno   p  = in_get_leaf (page_h_ro (pg), i - 1);
    const b_size b  = in_get_key (page_h_ro (pg), i - 1);
    const err_t  rc = nupd_observe_left (s, p, b, e);
    if (rc != SUCCESS) {
      return rc;
    }
  }

  return SUCCESS;
}

err_t
nupd_observe_all_right (struct node_updates *s, const page_h *pg, error *e)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->robs <= s->rlen);

  if (pg->mode == PHM_NONE) {
    s->robs = s->rlen;
    return SUCCESS;
  }

  ASSERT (page_h_type (pg) == PG_INNER_NODE);

  for (p_size i = 0; i < in_get_len (page_h_ro (pg)) && i < IN_MAX_KEYS; ++i) {
    const pgno   p  = in_get_leaf (page_h_ro (pg), i);
    const b_size b  = in_get_key (page_h_ro (pg), i);
    const err_t  rc = nupd_observe_right (s, p, b, e);
    if (rc != SUCCESS) {
      return rc;
    }
  }

  return SUCCESS;
}

err_t
nupd_observe_all_left (struct node_updates *s, const page_h *pg, error *e)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->lobs <= s->llen);

  if (pg->mode == PHM_NONE) {
    s->lobs = s->llen;
    return SUCCESS;
  }

  ASSERT (page_h_type (pg) == PG_INNER_NODE);

  for (p_size i = in_get_len (page_h_ro (pg)); i > 0; --i) {
    const pgno   p  = in_get_leaf (page_h_ro (pg), i - 1);
    const b_size b  = in_get_key (page_h_ro (pg), i - 1);
    const err_t  rc = nupd_observe_left (s, p, b, e);
    if (rc != SUCCESS) {
      return rc;
    }
  }

  return SUCCESS;
}

struct in_pair
nupd_consume_right (struct node_updates *s)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->rcons < s->robs);

  return *nupd_get_right (s, s->rcons++);
}

#ifndef NDEBUG
TEST (nupd_consume_right)
{
  TEST_CASE ("Consume single entry")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);
    n->robs                = 1;

    const struct in_pair p = nupd_consume_right (n);

    test_assert_equal (p.pg, 200);
    test_assert_equal (p.key, 1024);
    test_assert_equal (n->rcons, 1);

    nupd_free (n);
  }

  TEST_CASE ("Consume multiple entries in order")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);
    nupd_append_right (n, 300, 2048, &ctx->e);
    nupd_append_right (n, 400, 4096, &ctx->e);
    n->robs                 = 3;

    const struct in_pair p1 = nupd_consume_right (n);
    test_assert_equal (p1.pg, 200);
    test_assert_equal (n->rcons, 1);

    const struct in_pair p2 = nupd_consume_right (n);
    test_assert_equal (p2.pg, 300);
    test_assert_equal (n->rcons, 2);

    const struct in_pair p3 = nupd_consume_right (n);
    test_assert_equal (p3.pg, 400);
    test_assert_equal (n->rcons, 3);

    nupd_free (n);
  }

  TEST_CASE ("Consume across slab boundary")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    for (u32 i = 0; i < NUPD_LENGTH + 2; ++i) {
      nupd_append_right (n, 200 + i, 1000 + i, &ctx->e);
    }
    n->robs = NUPD_LENGTH + 2;

    for (u32 i = 0; i < NUPD_LENGTH + 2; ++i) {
      const struct in_pair p = nupd_consume_right (n);
      test_assert_equal (p.pg, 200 + i);
      test_assert_equal (p.key, 1000 + i);
    }

    nupd_free (n);
  }
}
#endif

struct in_pair
nupd_consume_left (struct node_updates *s)
{
  DBG_ASSERT (node_updates, s);
  ASSERT (s->lcons < s->lobs);

  return *nupd_get_left (s, s->lcons++);
}

#ifndef NDEBUG
TEST (nupd_consume_left)
{
  TEST_CASE ("Consume single entry")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);
    n->lobs                = 1;

    const struct in_pair p = nupd_consume_left (n);

    test_assert_equal (p.pg, 50);
    test_assert_equal (p.key, 256);
    test_assert_equal (n->lcons, 1);

    nupd_free (n);
  }

  TEST_CASE ("Consume multiple entries in order")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 90, 512, &ctx->e);
    nupd_append_left (n, 80, 256, &ctx->e);
    nupd_append_left (n, 70, 128, &ctx->e);
    n->lobs                 = 3;

    const struct in_pair p1 = nupd_consume_left (n);
    test_assert_equal (p1.pg, 90);
    test_assert_equal (n->lcons, 1);

    const struct in_pair p2 = nupd_consume_left (n);
    test_assert_equal (p2.pg, 80);
    test_assert_equal (n->lcons, 2);

    const struct in_pair p3 = nupd_consume_left (n);
    test_assert_equal (p3.pg, 70);
    test_assert_equal (n->lcons, 3);

    nupd_free (n);
  }

  TEST_CASE ("Consume across slab boundary")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    for (u32 i = 0; i < NUPD_LENGTH + 2; ++i) {
      nupd_append_left (n, 50 + i, 100 + i, &ctx->e);
    }
    n->lobs = NUPD_LENGTH + 2;

    for (u32 i = 0; i < NUPD_LENGTH + 2; ++i) {
      const struct in_pair p = nupd_consume_left (n);
      test_assert_equal (p.pg, 50 + i);
      test_assert_equal (p.key, 100 + i);
    }

    nupd_free (n);
  }
}
#endif

bool
nupd_done_observing_left (const struct node_updates *s)
{
  return s->lobs >= s->llen;
}

#ifndef NDEBUG
TEST (nupd_done_observing_left)
{
  TEST_CASE ("True when no left entries")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    test_assert_equal (nupd_done_observing_left (n), 1);

    nupd_free (n);
  }

  TEST_CASE ("False when left entries not observed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);

    test_assert_equal (nupd_done_observing_left (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("True when all left entries observed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);
    nupd_append_left (n, 25, 128, &ctx->e);
    n->lobs = 2;

    test_assert_equal (nupd_done_observing_left (n), 1);

    nupd_free (n);
  }

  TEST_CASE ("False when partially observed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);
    nupd_append_left (n, 25, 128, &ctx->e);
    n->lobs = 1;

    test_assert_equal (nupd_done_observing_left (n), 0);

    nupd_free (n);
  }
}
#endif

bool
nupd_done_observing_right (const struct node_updates *s)
{
  return s->robs >= s->rlen;
}

#ifndef NDEBUG
TEST (nupd_done_observing_right)
{
  TEST_CASE ("True when no right entries")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    test_assert_equal (nupd_done_observing_right (n), 1);

    nupd_free (n);
  }

  TEST_CASE ("False when right entries not observed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);

    test_assert_equal (nupd_done_observing_right (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("True when all right entries observed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);
    nupd_append_right (n, 300, 2048, &ctx->e);
    n->robs = 2;

    test_assert_equal (nupd_done_observing_right (n), 1);

    nupd_free (n);
  }
}
#endif

bool
nupd_done_consuming_left (const struct node_updates *s)
{
  return s->lcons == s->lobs;
}

#ifndef NDEBUG
TEST (nupd_done_consuming_left)
{
  TEST_CASE ("True when nothing to consume")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    test_assert_equal (nupd_done_consuming_left (n), 1);

    nupd_free (n);
  }

  TEST_CASE ("False when observed but not consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);
    n->lobs = 1;

    test_assert_equal (nupd_done_consuming_left (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("True when fully consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);
    n->lobs  = 1;
    n->lcons = 1;

    test_assert_equal (nupd_done_consuming_left (n), 1);

    nupd_free (n);
  }
}
#endif

bool
nupd_done_consuming_right (const struct node_updates *s)
{
  return s->rcons == s->robs;
}

#ifndef NDEBUG
TEST (nupd_done_consuming_right)
{
  TEST_CASE ("True when nothing to consume")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    test_assert_equal (nupd_done_consuming_right (n), 1);

    nupd_free (n);
  }

  TEST_CASE ("False when observed but not consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);
    n->robs = 1;

    test_assert_equal (nupd_done_consuming_right (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("True when fully consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);
    n->robs  = 1;
    n->rcons = 1;

    test_assert_equal (nupd_done_consuming_right (n), 1);

    nupd_free (n);
  }
}
#endif

bool
nupd_done_left (struct node_updates *s)
{
  return (nupd_done_observing_left (s) && nupd_done_consuming_left (s)) != 0;
}

#ifndef NDEBUG
TEST (nupd_done_left)
{
  TEST_CASE ("True for empty node")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    test_assert_equal (nupd_done_left (n), 1);

    nupd_free (n);
  }

  TEST_CASE ("False when not observed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);

    test_assert_equal (nupd_done_left (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("False when observed but not consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);
    n->lobs = 1;

    test_assert_equal (nupd_done_left (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("True when observed and consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_left (n, 50, 256, &ctx->e);
    n->lobs  = 1;
    n->lcons = 1;

    test_assert_equal (nupd_done_left (n), 1);

    nupd_free (n);
  }
}
#endif

bool
nupd_done_right (struct node_updates *s)
{
  return (nupd_done_observing_right (s) && nupd_done_consuming_right (s)) != 0;
}

#ifndef NDEBUG
TEST (nupd_done_right)
{
  TEST_CASE ("True for empty node")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);

    test_assert_equal (nupd_done_right (n), 1);

    nupd_free (n);
  }

  TEST_CASE ("False when not observed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);

    test_assert_equal (nupd_done_right (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("False when observed but not consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);
    n->robs = 1;

    test_assert_equal (nupd_done_right (n), 0);

    nupd_free (n);
  }

  TEST_CASE ("True when observed and consumed")
  {
    struct node_updates *n = nupd_init (100, 512, ctx->mem, &ctx->e);
    nupd_append_right (n, 200, 1024, &ctx->e);
    n->robs  = 1;
    n->rcons = 1;

    test_assert_equal (nupd_done_right (n), 1);

    nupd_free (n);
  }
}
#endif

p_size
nupd_append_maximally_left (struct node_updates *n, const page_h *pg, const p_size lidx)
{
  DBG_ASSERT (node_updates, n);
  ASSERT (page_h_type (pg) == PG_INNER_NODE);
  ASSERT (in_get_len (page_h_ro (pg)) == IN_MAX_KEYS);

  p_size ret = 0;
  p_size i   = lidx;

  while (i > 0 && n->lcons < n->lobs) {
    const struct in_pair next = nupd_consume_left (n);
    if (next.key > 0) {
      in_set_key_leaf (page_h_w (pg), i - 1, next.key, next.pg);
      i--;
      ret++;
    }
  }

  return ret;
}

p_size
nupd_append_maximally_right (struct node_updates *n, const page_h *pg, const p_size lidx)
{
  DBG_ASSERT (node_updates, n);
  ASSERT (page_h_type (pg) == PG_INNER_NODE);
  ASSERT (in_get_len (page_h_ro (pg)) == IN_MAX_KEYS);

  p_size ret = 0;
  p_size i   = lidx;

  while (i < IN_MAX_KEYS && n->rcons < n->robs) {
    const struct in_pair next = nupd_consume_right (n);
    if (next.key > 0) {
      in_set_key_leaf (page_h_w (pg), i, next.key, next.pg);
      i++;
      ret++;
    }
  }

  return ret;
}

p_size
nupd_append_maximally_right_then_left (struct node_updates *n, page_h *pg)
{
  DBG_ASSERT (node_updates, n);
  ASSERT (page_h_type (pg) == PG_INNER_NODE);
  ASSERT (in_get_len (page_h_ro (pg)) == IN_MAX_KEYS);

  // Append Pivot to the front
  // [+-------------------]
  // ^
  // len = 1
  p_size len = 0;
  if (n->pivot.key > 0) {
    in_set_key_leaf (page_h_w (pg), len++, n->pivot.key, n->pivot.pg);
  }

  // Apply right
  // [++++++++++++++------]
  // ^
  // len
  len += nupd_append_maximally_right (n, pg, len);

  ASSERT (len <= IN_MAX_KEYS);

  // Haven't finish - apply right remaining pages
  if (len < IN_MAX_KEYS) {
    // Shift right (memcpy x1)
    // [------++++++++++++++]
    // ^
    // IN_MAX_KEYS - len - 1
    in_push_left_permissive (page_h_w (pg), len);

    // Apply left
    // [--++++++++++++++++++]
    len += nupd_append_maximally_left (n, pg, IN_MAX_KEYS - len);
  }

  return len;
}
