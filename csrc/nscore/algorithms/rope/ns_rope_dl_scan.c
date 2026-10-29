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
#include "nscore/page/ns_page_fixture.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/pager/ns_pager.h"

#ifndef NDEBUG
#  include "core/ns_testing.h" // TEST
#endif

#include <stdbool.h>

err_t
ns_rope_dl_scan (struct pager *p, const page_h *cur, page_consumer consume, void *ctx, error *e)
{
  DBG_ASSERT (pager, p);
  DBG_ASSERT (page_h, cur);

  // Consume the first page
  struct data_list dl = dl_get (page_h_ro (cur));
  if (consume (dl, ctx, e) < 0) {
    return error_trace (e);
  }

  // Fetch next page
  page_h iter = page_h_create ();
  if (dl.npg != PGNO_NULL) {
    if (pgr_get (&iter, PG_DATA_LIST, dl.npg, p, e)) {
      return error_trace (e);
    }
  }

  while (true) {
    // Done
    if (iter.mode == PHM_NONE) {
      return SUCCESS;
    }

    // Get data
    dl = dl_get (page_h_ro (&iter));

    // Execute consumer
    if (consume (dl, ctx, e) < 0) {
      pgr_release (p, &iter, PG_DATA_LIST, e);
      return error_trace (e);
    }

    // Release current
    if (pgr_release (p, &iter, PG_DATA_LIST, e)) {
      return error_trace (e);
    }

    // Fetch next
    if (dl.npg != PGNO_NULL) {
      if (pgr_get (&iter, PG_DATA_LIST, dl.npg, p, e)) {
        return error_trace (e);
      }
    }
  }
}

err_t
ns_rope_dl_scan_pgno (struct pager *p, pgno pg, page_consumer consume, void *ctx, error *e)
{
  page_h cur = page_h_create ();
  if (pgr_get (&cur, PG_DATA_LIST, pg, p, e) < 0) {
    return error_trace (e);
  }

  if (ns_rope_dl_scan (p, &cur, consume, ctx, e) < 0) {
    pgr_release (p, &cur, PG_DATA_LIST, e);
    return error_trace (e);
  }

  return pgr_release (p, &cur, PG_DATA_LIST, e);
}

////////////////// BLEN

struct blen_ctx
{
  b_size running_total;
};

static err_t
ns_rope_dl_blen_consumer (struct data_list dl, void *_ctx, MAYBE_UNUSED error *e)
{
  struct blen_ctx *ctx = _ctx;
  ctx->running_total += dl.dlen;
  return SUCCESS;
}

sb_size
ns_rope_dl_scan_blen (struct pager *p, page_h *cur, error *e)
{
  struct blen_ctx ctx = {
      .running_total = 0,
  };

  if (ns_rope_dl_scan (p, cur, ns_rope_dl_blen_consumer, &ctx, e) < 0) {
    return error_trace (e);
  }

  return ctx.running_total;
}

sb_size
ns_rope_dl_scan_blen_pgno (struct pager *p, pgno pg, error *e)
{
  page_h cur = page_h_create ();
  if (pgr_get (&cur, PG_DATA_LIST, pg, p, e) < 0) {
    return error_trace (e);
  }

  sb_size len = ns_rope_dl_scan_blen (p, &cur, e);
  if (len < 0) {
    pgr_release (p, &cur, PG_DATA_LIST, e);
    return error_trace (e);
  }

  if (pgr_release (p, &cur, PG_DATA_LIST, e) < 0) {
    return error_trace (e);
  }

  return len;
}

////////////////// NPages

struct npages_ctx
{
  b_size running_total;
};

static err_t
ns_rope_dl_npages_consumer (MAYBE_UNUSED struct data_list dl, void *_ctx, MAYBE_UNUSED error *e)
{
  struct blen_ctx *ctx = _ctx;
  ctx->running_total += 1;
  return SUCCESS;
}

sb_size
ns_rope_dl_scan_npages (struct pager *p, page_h *cur, error *e)
{
  struct npages_ctx ctx = {
      .running_total = 0,
  };

  if (ns_rope_dl_scan (p, cur, ns_rope_dl_npages_consumer, &ctx, e) < 0) {
    return error_trace (e);
  }

  return ctx.running_total;
}

#ifndef NDEBUG

/******************************************************************************
 * SECTION: Tests
 ******************************************************************************/

/// Packs [remaining] bytes into as few data-list pages as possible. Page
/// contents are the page index repeated, so a scan that visits pages out of
/// order or revisits one is visible in the data rather than just the count:
///
///   page 0      page 1      page 2
///   [000...0]   [111...1]   [222..2]
///
/// Every non-root page must stay on or above half capacity (dl_validate_for_db
/// runs at release), so callers pick lengths whose tail page lands there.
struct dl_fill_ctx
{
  u32 remaining;
  u32 npages;
};

static err_t
dl_fill (page *cur, void *_c, bool *done, MAYBE_UNUSED error *e)
{
  struct dl_fill_ctx *c     = _c;
  const p_size        avail = dl_avail (cur);
  p_size              take  = (p_size)c->remaining;

  if (take > avail) {
    take = avail;
  }

  u8 byte[1] = {(u8)c->npages};
  for (p_size i = 0; i < take; ++i) {
    dl_append (cur, byte, 1);
  }

  c->remaining -= take;
  c->npages++;
  *done = (c->remaining == 0);
  return SUCCESS;
}

/// Builds a committed chain of exactly [npages] pages and returns its head.
static pgno
dl_build_chain (struct pgr_fixture *f, const u32 npages, u32 *blen_out, error *e)
{
  // Full pages, except a tail at exactly half so the page count is exact.
  const u32          blen = (npages - 1) * DL_DATA_SIZE + (npages == 1 ? 1 : DL_DATA_SIZE / 2);

  struct dl_fill_ctx c    = {.remaining = blen, .npages = 0};

  pgr_begin_txn (&f->tx, f->p, e);
  const spgno root = ns_rope_dl_init (f->p, &f->tx, dl_fill, &c, e);
  ASSERT (root >= 0);
  pgr_commit (f->p, &f->tx, e);

  ASSERT (c.npages == npages);
  *blen_out = blen;
  return (pgno)root;
}

/// Records what a scan saw, so order and page identity can be asserted and
/// not just totals.
struct dl_record_ctx
{
  u32    nvisited;
  p_size dlen[16];
  u8     first_byte[16];
  pgno   npg[16];
  pgno   ppg[16];

  // Fail from the consumer on this visit index, or -1.
  i32    fail_on_visit;
};

static err_t
dl_record (struct data_list dl, void *_c, error *e)
{
  struct dl_record_ctx *c = _c;

  if (c->fail_on_visit >= 0 && (i32)c->nvisited == c->fail_on_visit) {
    return error_causef (e, ERR_IO, "injected consumer failure");
  }

  ASSERT (c->nvisited < 16);
  const u32 i      = c->nvisited++;

  c->dlen[i]       = dl.dlen;
  c->first_byte[i] = dl.dlen > 0 ? ((const u8 *)dl.data)[0] : 0xff;
  c->npg[i]        = dl.npg;
  c->ppg[i]        = dl.ppg;

  return SUCCESS;
}

/// A forward scan visits every page of the chain exactly once, in order,
/// starting at the page it was handed. The scan follows npg, so ppg is checked
/// against each neighbour too - counts alone cannot tell the two apart.
///
/// Each row is a chain length: 1 page (the degenerate chain, npg == PGNO_NULL
/// on the only page), 2 (the first real link) and 6 (several hops).
TEST (ns_rope_dl_scan_pgno)
{
  const u32 lengths[] = {1, 2, 6};

  for (u32 k = 0; k < 3; ++k) {
    TEST_CASE ("chain of %u page(s)", lengths[k])
    {
      struct pgr_fixture f;
      pgr_fixture_create (&f, &ctx->e);

      u32                  blen = 0;
      const pgno           root = dl_build_chain (&f, lengths[k], &blen, &ctx->e);

      struct dl_record_ctx c    = {.fail_on_visit = -1};
      test_assert_int_equal (ns_rope_dl_scan_pgno (f.p, root, dl_record, &c, &ctx->e), SUCCESS);

      test_assert_int_equal (c.nvisited, lengths[k]);

      u32 total = 0;
      for (u32 i = 0; i < c.nvisited; ++i) {
        // Visited in build order.
        test_assert_int_equal (c.first_byte[i], (u8)i);
        total += c.dlen[i];

        // The chain is doubly linked and consistent across neighbours.
        if (i == 0) {
          test_assert (c.ppg[i] == PGNO_NULL);
        }
        if (i + 1 == c.nvisited) {
          test_assert (c.npg[i] == PGNO_NULL);
        } else {
          test_assert (c.npg[i] != PGNO_NULL);
        }
      }
      test_assert_int_equal (total, blen);

      pgr_fixture_teardown (&f, &ctx->e);
    }
  }
}

/// blen and npages are both ns_rope_dl_scan with a different consumer. Both
/// have to agree with the chain that was built, and the page_h and pgno entry
/// points have to agree with each other.
TEST (ns_rope_dl_scan_blen_and_npages)
{
  struct pgr_fixture f;
  pgr_fixture_create (&f, &ctx->e);

  const u32 lengths[] = {1, 3};

  for (u32 k = 0; k < 2; ++k) {
    TEST_CASE ("chain of %u page(s)", lengths[k])
    {
      u32        blen = 0;
      const pgno root = dl_build_chain (&f, lengths[k], &blen, &ctx->e);

      // The pgno entry point.
      test_assert_int_equal (ns_rope_dl_scan_blen_pgno (f.p, root, &ctx->e), blen);

      // The page_h entry point has to give the same answer.
      page_h h = page_h_create ();
      test_assert (pgr_get (&h, PG_DATA_LIST, root, f.p, &ctx->e) >= 0);
      test_assert_int_equal (ns_rope_dl_scan_blen (f.p, &h, &ctx->e), blen);
      test_assert_int_equal (ns_rope_dl_scan_npages (f.p, &h, &ctx->e), lengths[k]);
      test_assert (pgr_release (f.p, &h, PG_DATA_LIST, &ctx->e) >= 0);
    }
  }

  pgr_fixture_teardown (&f, &ctx->e);
}

/// A consumer that fails has to stop the scan and surface the failure, and the
/// page the scan was holding has to be released on the way out. An unreleased
/// handle is caught by pgr_close during teardown, so the fixture is the
/// assertion for the leak.
///
/// The visit index matters: failing on visit 0 happens before the loop is
/// entered at all (the first page is consumed by the caller's own handle),
/// while a later visit fails inside the loop with a page the scan itself
/// fetched and therefore owns.
TEST (ns_rope_dl_scan_consumer_failure)
{
  const i32 fail_visits[] = {0, 1, 3};

  for (u32 k = 0; k < 3; ++k) {
    TEST_CASE ("consumer fails on visit %d", fail_visits[k])
    {
      struct pgr_fixture f;
      pgr_fixture_create (&f, &ctx->e);

      u32                  blen = 0;
      const pgno           root = dl_build_chain (&f, 6, &blen, &ctx->e);

      struct dl_record_ctx c    = {.fail_on_visit = fail_visits[k]};
      test_assert (ns_rope_dl_scan_pgno (f.p, root, dl_record, &c, &ctx->e) < 0);
      test_assert (ctx->e.cause_code < 0);
      error_reset (&ctx->e);

      // Stopped where it was told to, rather than carrying on.
      test_assert_int_equal (c.nvisited, (u32)fail_visits[k]);

      // The pager is still usable, so nothing was left fixed.
      test_assert_int_equal (ns_rope_dl_scan_blen_pgno (f.p, root, &ctx->e), blen);

      pgr_fixture_teardown (&f, &ctx->e);
    }
  }
}

#endif // NDEBUG
