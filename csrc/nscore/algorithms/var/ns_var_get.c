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

#include "core/ns_error.h"
#include "nscore/algorithms/var/ns_var_algorithms.h"
#include "nscore/algorithms/var/ns_var_algorithms_internal.h"
#include "nscore/page/ns_page.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/pager/ns_pager.h"

#include <stdbool.h>
#include <stddef.h>

err_t
ns_var_get (struct ns_var_get_params *params, error *e)
{
  page_h                         cur     = page_h_create ();

  // Find variable first
  struct ns_find_var_page_params fparams = {
      .p     = params->p,
      .tx    = params->tx,
      .alloc = params->alloc,

      .vname = params->vname,
      .dvar  = &params->dest, // Dest
      .mode  = FP_FIND,

      .prev  = NULL,
      .cur   = &cur,
  };

  if (ns_find_var_page (&fparams, e)) {
    goto theend;
  }

  // Read the variable here
  struct ns_read_var_page_params rparams = {
      .p          = params->p,
      .tx         = params->tx,

      .vp         = &cur,
      .alloc      = params->alloc,

      .matches    = true,
      .check      = NULL,

      .dest       = &params->dest,

      .save_vname = false,
      .save_type  = true,
  };
  if (ns_read_var_page (&rparams, e)) {
    goto theend;
  }

  // TODO - validate variable here

  if (pgr_release (params->p, &cur, PG_VAR_PAGE, e)) {
    goto theend;
  }

theend:
  pgr_cancel_if_exists (&cur);

  return error_trace (e);
}
