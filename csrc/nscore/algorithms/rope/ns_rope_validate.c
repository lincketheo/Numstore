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
#include "nscore/page/ns_page_fixture.h"
#include "nscore/pager/ns_pager.h"

#include <stdbool.h>

err_t
ns_rope_validate (struct pager *p, pgno root, error *e)
{
  if (root == PGNO_NULL) {
    return SUCCESS;
  }

  // Get all the layer page numbers
  pgno  layers[20];
  spgno nlayers = ns_rope_get_layers (p, layers, root, e);
  if (nlayers < 0) {
    return error_trace (e);
  }
  ASSERT (nlayers > 0);

  // For each inner node layer - validate it:
  for (u32 i = 0; i < nlayers - 1; ++i) {
    if (ns_rope_in_validate_pgno (p, layers[i], e) < 0) {
      return error_trace (e);
    }
  }

  // For the final data list layer - validate it
  sb_size blen = ns_rope_dl_scan_blen_pgno (p, layers[nlayers - 1], e);
  if (blen < 0) {
    return error_trace (e);
  }

  // Get root page stats
  struct rope_page_stats stats;
  if (ns_rope_page_stats (p, &stats, layers[0], e) < 0) {
    return error_trace (e);
  }

  if ((b_size)blen != stats.tracked) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "Bottom layer has: %" PRb_size " bytes while root has: %" PRb_size " bytes",
        blen,
        stats.tracked
    );
  }

  return SUCCESS;
}
