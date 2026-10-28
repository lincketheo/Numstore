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
ns_rope_dl_blen_consumer (struct data_list dl, void *_ctx, error *e)
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
ns_rope_dl_npages_consumer (struct data_list dl, void *_ctx, error *e)
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
