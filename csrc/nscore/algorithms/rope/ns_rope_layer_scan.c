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
#include "nscore/page/ns_page_inner_node.h"
#include "nscore/pager/ns_pager.h"

#include <stdbool.h>

i32
ns_rope_get_layers (struct pager *p, pgno dest[20], pgno root, error *e)
{
  if (root == PGNO_NULL) {
    return 0;
  }

  page_h next = page_h_create ();
  page_h cur  = page_h_create ();

  // Get the first page
  if (pgr_get (&cur, PG_INNER_NODE | PG_DATA_LIST, root, p, e) < 0) {
    return error_trace (e);
  }

  u32 sp = 0;

  while (true) {
    // Stack overflow
    if (sp == 20) {
      error_causef (e, ERR_RPTREE_PAGE_STACK_OVERFLOW, "page stack overflow (depth 20)");
      goto failed;
    }

    dest[sp++] = page_h_pgno (&cur);

    switch (page_h_type (&cur)) {
      case PG_INNER_NODE: {
        // Get the left most leaf
        const pgno npg = in_get_leaf (page_h_ro (&cur), 0);
        if (pgr_get (&next, PG_INNER_NODE | PG_DATA_LIST, npg, p, e) < 0) {
          goto failed;
        }

        // Release current
        if (pgr_release (p, &cur, PG_INNER_NODE, e) < 0) {
          goto failed;
        }

        // Trade pages
        cur = page_h_xfer_ownership (&next);

        break;
      }
      case PG_DATA_LIST: {
        // We're at the bottom layer - so we're done
        if (pgr_release (p, &cur, PG_INNER_NODE, e) < 0) {
          goto failed;
        }
        return sp;
      }

      default: {
        UNREACHABLE (); // LCOV_EXCL_LINE
      }
    }
  }

failed:
  pgr_cancel_if_exists (&next);
  pgr_cancel_if_exists (&cur);

  return error_trace (e);
}
