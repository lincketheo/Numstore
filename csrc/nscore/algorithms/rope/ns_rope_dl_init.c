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
