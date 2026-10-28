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
#include "nscore/page/ns_page_fixture.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/pager/ns_pager.h"

#include <stdbool.h>

err_t
ns_rope_in_scan (struct pager *p, const page_h *cur, in_page_consumer consume, void *ctx, error *e)
{
  DBG_ASSERT (pager, p);
  DBG_ASSERT (page_h, cur);

  struct inner_node in;

  // Consume the first page
  in_get (&in, page_h_ro (cur));
  if (consume (&in, ctx, e) < 0) {
    return error_trace (e);
  }

  // Fetch next page
  page_h iter = page_h_create ();
  if (in.npg != PGNO_NULL) {
    if (pgr_get (&iter, PG_DATA_LIST, in.npg, p, e)) {
      return error_trace (e);
    }
  }

  while (true) {
    // Done
    if (iter.mode == PHM_NONE) {
      return SUCCESS;
    }

    // Get data
    in_get (&in, page_h_ro (&iter));

    // Execute consumer
    if (consume (&in, ctx, e) < 0) {
      pgr_release (p, &iter, PG_DATA_LIST, e);
      return error_trace (e);
    }

    // Release current
    if (pgr_release (p, &iter, PG_DATA_LIST, e)) {
      return error_trace (e);
    }

    // Fetch next
    if (in.npg != PGNO_NULL) {
      if (pgr_get (&iter, PG_DATA_LIST, in.npg, p, e)) {
        return error_trace (e);
      }
    }
  }
}

err_t
ns_rope_in_scan_pgno (struct pager *p, pgno pg, in_page_consumer consume, void *ctx, error *e)
{
  page_h cur = page_h_create ();
  if (pgr_get (&cur, PG_INNER_NODE, pg, p, e) < 0) {
    return error_trace (e);
  }

  if (ns_rope_in_scan (p, &cur, consume, ctx, e) < 0) {
    pgr_release (p, &cur, PG_INNER_NODE, e);
    return error_trace (e);
  }

  return pgr_release (p, &cur, PG_INNER_NODE, e);
}

////////////////// Validate

static err_t
in_validate_consumer (struct inner_node *in, void *p, error *e)
{
  for (p_size i = 0; i < in->plen; ++i) {
    struct in_pair         pair = in->pairs[i];

    // Get root page stats
    struct rope_page_stats stats;
    if (ns_rope_page_stats (p, &stats, pair.pg, e) < 0) {
      return error_trace (e);
    }

    // Validate that the tracked bytes are the same
    if (stats.tracked != pair.key) {
      return error_causef (
          e,
          ERR_CORRUPT,
          "Inner node tracks: %" PRb_size " bytes but actual page tracks: %" PRb_size " bytes\n",
          pair.key,
          stats.tracked
      );
    }
  }

  return SUCCESS;
}

err_t
ns_rope_in_validate_pgno (struct pager *p, pgno pg, error *e)
{
  return ns_rope_in_scan_pgno (p, pg, in_validate_consumer, p, e);
}
