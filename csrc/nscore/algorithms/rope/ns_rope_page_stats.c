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
#include "nscore/page/ns_page_delegate.h"
#include "nscore/page/ns_page_fixture.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/pager/ns_pager.h"

#include <stdbool.h>

err_t
ns_rope_page_stats (struct pager *p, struct rope_page_stats *dest, pgno pg, error *e)
{
  DBG_ASSERT (pager, p);
  ASSERT (dest);
  ASSERT (pg != PGNO_NULL);

  page_h cur = page_h_create ();

  if (pgr_get (&cur, PG_INNER_NODE | PG_DATA_LIST, pg, p, e) < 0) {
    return error_trace (e);
  }

  struct rope_page_stats res = {
      .type    = page_h_type (&cur),
      .pg      = pg,
      .tracked = dlgt_get_size (page_h_ro (&cur)),
      .npg     = dlgt_get_next (page_h_ro (&cur)),
      .ppg     = dlgt_get_prev (page_h_ro (&cur)),
      .is_root = dlgt_is_root (page_h_ro (&cur)),
      .len     = dlgt_get_len (page_h_ro (&cur)),
      .cap     = dlgt_get_max_len (page_h_ro (&cur)),
  };

  if (pgr_release (p, &cur, dest->type, e) < 0) {
    pgr_cancel (&cur);
    return error_trace (e);
  }

  *dest = res;

  return SUCCESS;
}
