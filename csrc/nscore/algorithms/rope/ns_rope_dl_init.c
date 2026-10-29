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

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_stdtypes.h"
#include "nscore/algorithms/rope/ns_rope_algorithms.h"
#include "nscore/page/ns_page.h"
#include "nscore/page/ns_page_data_list.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/pager/ns_pager.h"

#ifndef NDEBUG
#  include "core/ns_numerics.h"            // rand_bytes
#  include "core/ns_testing.h"             // TEST
#  include "nscore/page/ns_page_fixture.h" // pgr_fixture

#  include <string.h>
#endif

#include <stdbool.h>

spgno
ns_rope_dl_init (struct pager *p, struct txn *tx, page_initializer init, void *ctx, error *e)
{
  DBG_ASSERT (pager, p);
  DBG_ASSERT (ns_txn, tx);

  page_h cur  = page_h_create ();
  page_h next = page_h_create ();

  if (pgr_new (&cur, p, tx, PG_DATA_LIST, e) < 0) {
    return error_trace (e);
  }

  // Save this
  spgno ret  = page_h_pgno (&cur);
  bool  done = false;

  while (true) {
    // Run init on this page
    if (init (page_h_w (&cur), ctx, &done, e) < 0) {
      goto failed;
    }

    // If we're complete - finish up
    if (done) {
      if (pgr_release (p, &cur, PG_DATA_LIST, e) < 0) {
        goto failed;
      }
      return ret;
    }

    // Move to the next page
    if (pgr_new (&next, p, tx, PG_DATA_LIST, e) < 0) {
      goto failed;
    }
    dl_set_next (page_h_w (&cur), page_h_pgno (&next));
    dl_set_prev (page_h_w (&next), page_h_pgno (&cur));

    // Release and swap
    if (pgr_release (p, &cur, PG_DATA_LIST, e) < 0) {
      goto failed;
    }
    page_h_xfer_ownership_ptr (&cur, &next);
  }

failed:
  pgr_cancel_if_exists (&cur);
  pgr_cancel_if_exists (&next);
  return error_trace (e);
}

spgno
ns_rope_dl_init_from (struct pager *p, void *data, b_size dlen, error *e)
{
  return error_causef (e, ERR_INVALID_ARGUMENT, "Unimplemented");
}

#ifndef NDEBUG

/******************************************************************************
 * SECTION: Tests
 ******************************************************************************/

/// Feeds a byte buffer into successive data-list pages, DL_DATA_SIZE at a
/// time, and reports done once the buffer is spent:
///
///   [########|########|###_____]
///    page 0   page 1   page 2
///                         ^
///                        done
///
/// pgr_new has already run page_init_empty, so the initializer only appends -
/// re-initializing here would wipe the prev link ns_rope_dl_init just set.
struct dl_init_test_ctx
{
  const u8 *src;
  u32       len;
  u32       written;
  u32       npages;

  // Inject a failure from the initializer on this page index, or -1.
  i32       fail_on_page;
};

static err_t
dl_init_test_fn (page *cur, void *_c, bool *done, error *e)
{
  struct dl_init_test_ctx *c = _c;

  if (c->fail_on_page >= 0 && (i32)c->npages == c->fail_on_page) {
    return error_causef (e, ERR_IO, "injected initializer failure");
  }

  const p_size avail = dl_avail (cur);
  p_size       take  = (p_size)(c->len - c->written);
  if (take > avail) {
    take = avail;
  }

  dl_append (cur, c->src + c->written, take);
  c->written += take;
  c->npages++;

  *done = (c->written == c->len);
  return SUCCESS;
}

/// Walks the chain from [root] and checks it is a well formed doubly linked
/// list holding exactly [expect] - forward links, backward links and content
/// all have to agree, since dl_init writes next on one page and prev on the
/// next and a swapped pair would still scan forward correctly.
static void
assert_chain_is (
    struct pager *p,
    const pgno    root,
    const u8     *expect,
    const u32     len,
    const u32     npages,
    error        *e
)
{
  pgno seen[64];
  u32  nseen  = 0;
  u32  offset = 0;
  pgno prev   = PGNO_NULL;
  pgno cur    = root;

  while (cur != PGNO_NULL) {
    page_h h = page_h_create ();
    ASSERT (pgr_get (&h, PG_DATA_LIST, cur, p, e) >= 0);

    const struct data_list dl = dl_get (page_h_ro (&h));

    ASSERT (nseen < 64);
    seen[nseen++] = cur;

    ASSERT (dl.ppg == prev);          // backward link
    ASSERT (offset + dl.dlen <= len); // no page overruns the input
    ASSERT (memcmp (dl.data, expect + offset, dl.dlen) == 0);
    offset += dl.dlen;

    prev = cur;
    cur  = dl.npg;

    ASSERT (pgr_release (p, &h, PG_DATA_LIST, e) >= 0);
  }

  ASSERT (nseen == npages);
  ASSERT (offset == len);
}

/// Each row is a payload length, chosen around the page boundary because that
/// is where the "one more page" transition lives:
///
///   < cap      -> done on page 0, no link written at all
///   == cap     -> done on page 0 (the initializer spends the buffer exactly)
///   cap + half -> first time a second page and a link are created
///   n * cap    -> last page ends exactly full
///
/// Lengths are not free: pgr_release runs dl_validate_for_db, which requires
/// every non-root page to hold at least DL_DATA_SIZE / 2 bytes. So a greedy
/// initializer can only build chains whose tail page lands on or above half:
///
///   [########|####____]   ok, tail >= half
///   [########|#_______]   rejected at release, not at init
///
/// A tail below half aborts in pgr_release rather than returning an error, so
/// that case cannot be covered from here.
#  define DL_INIT_CASE(label, length, pages)                                           \
    do {                                                                               \
      TEST_CASE ("%s: %u bytes over %u page(s)", label, (u32)(length), (u32)(pages))   \
      {                                                                                \
        u8 *src = ctx->mem.table->malloc (ctx->mem.self, 1, (length), &ctx->e);        \
        test_assert (src != NULL);                                                     \
        rand_bytes (src, (length));                                                    \
                                                                                       \
        struct dl_init_test_ctx c = {                                                  \
            .src          = src,                                                       \
            .len          = (length),                                                  \
            .fail_on_page = -1,                                                        \
        };                                                                             \
                                                                                       \
        pgr_begin_txn (&f.tx, f.p, &ctx->e);                                           \
        const spgno root = ns_rope_dl_init (f.p, &f.tx, dl_init_test_fn, &c, &ctx->e); \
        test_assert (root >= 0);                                                       \
        pgr_commit (f.p, &f.tx, &ctx->e);                                              \
                                                                                       \
        test_assert_int_equal (c.npages, (pages));                                     \
        test_assert_int_equal (c.written, (length));                                   \
                                                                                       \
        /* The returned pgno must be the head, not the tail. */                        \
        assert_chain_is (f.p, (pgno)root, src, (length), (pages), &ctx->e);            \
                                                                                       \
        ctx->mem.table->free (ctx->mem.self, src);                                     \
      }                                                                                \
    }                                                                                  \
    while (0)

TEST (ns_rope_dl_init)
{
  struct pgr_fixture f;
  pgr_fixture_create (&f, &ctx->e);

  // A lone page is the root, so it may hold as little as one byte.
  DL_INIT_CASE ("one byte", 1, 1);
  DL_INIT_CASE ("just under one page", DL_DATA_SIZE - 1, 1);
  DL_INIT_CASE ("exactly one page", DL_DATA_SIZE, 1);

  // Multi page: the tail has to stay on or above half capacity.
  // Multi page: the tail has to stay on or above half capacity.
  DL_INIT_CASE ("tail exactly at half", DL_DATA_SIZE + DL_DATA_SIZE / 2, 2);
  DL_INIT_CASE ("exactly two pages", 2 * DL_DATA_SIZE, 2);
  DL_INIT_CASE ("six pages, partial tail", 5 * DL_DATA_SIZE + 3000, 6);

  pgr_fixture_teardown (&f, &ctx->e);
}

/// When the initializer fails, dl_init has to cancel every page handle it is
/// holding and report the failure, leaving a transaction that can still be
/// rolled back.
///
/// The rollback is not optional cleanup: pgr_begin_txn holds the DB lock
/// exclusively for the life of the transaction, so tearing the fixture down
/// with the failed transaction still open blocks pgr_close forever instead of
/// failing. Each case gets its own fixture so one deadlock cannot be hidden
/// by a later case.
#  define DL_INIT_FAILURE_CASE(fail_page, expect_pages)                                \
    do {                                                                               \
      TEST_CASE ("initializer fails on page %d", (fail_page))                          \
      {                                                                                \
        struct pgr_fixture f;                                                          \
        pgr_fixture_create (&f, &ctx->e);                                              \
                                                                                       \
        const u32 len = 5 * DL_DATA_SIZE;                                              \
        u8       *src = ctx->mem.table->malloc (ctx->mem.self, 1, len, &ctx->e);       \
        test_assert (src != NULL);                                                     \
        rand_bytes (src, len);                                                         \
                                                                                       \
        struct dl_init_test_ctx c = {                                                  \
            .src          = src,                                                       \
            .len          = len,                                                       \
            .fail_on_page = (fail_page),                                               \
        };                                                                             \
                                                                                       \
        pgr_begin_txn (&f.tx, f.p, &ctx->e);                                           \
        const spgno root = ns_rope_dl_init (f.p, &f.tx, dl_init_test_fn, &c, &ctx->e); \
                                                                                       \
        test_assert (root < 0);                                                        \
        test_assert (ctx->e.cause_code < 0);                                           \
        error_reset (&ctx->e);                                                         \
                                                                                       \
        /* The failure came from the page the test aimed at, not an earlier */         \
        /* one, so the unwind really did run with a chain behind it. */                \
        test_assert_int_equal (c.npages, (expect_pages));                              \
                                                                                       \
        test_assert_int_equal (pgr_rollback (f.p, &f.tx, 0, &ctx->e), SUCCESS);        \
                                                                                       \
        ctx->mem.table->free (ctx->mem.self, src);                                     \
        pgr_fixture_teardown (&f, &ctx->e);                                            \
      }                                                                                \
    }                                                                                  \
    while (0)

TEST (ns_rope_dl_init_initializer_failure)
{
  // Page 0 fails with only [cur] held and no link written yet; page 2 fails
  // with two pages already linked and released behind it.
  DL_INIT_FAILURE_CASE (0, 0);
  DL_INIT_FAILURE_CASE (2, 2);
}

/// Unimplemented, and callers have to be told rather than handed a page
/// number. This pins the contract so the stub cannot quietly start returning
/// a bogus pgno.
TEST (ns_rope_dl_init_from_is_unimplemented)
{
  struct pgr_fixture f;
  pgr_fixture_create (&f, &ctx->e);

  u8 data[16] = {0};
  test_assert (ns_rope_dl_init_from (f.p, data, sizeof (data), &ctx->e) < 0);
  test_assert_int_equal (ctx->e.cause_code, ERR_INVALID_ARGUMENT);
  error_reset (&ctx->e);

  pgr_fixture_teardown (&f, &ctx->e);
}

#endif // NDEBUG
