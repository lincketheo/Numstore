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
#include "core/ns_stream.h"
#include "core/ns_utils.h"
#include "nscore/algorithms/rope/ns_rope_algorithms.h"
#include "nscore/algorithms/rope/ns_rope_algorithms_internal.h"
#include "nscore/page/ns_page.h"
#include "nscore/page/ns_page_data_list.h"
#include "nscore/page/ns_page_delegate.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/pager/ns_pager.h"

#include <stdbool.h>
#include <stddef.h>

/******************************************************************************
 * SECTION: ns_rope_read
 * ----------------------------------------------------------------------------
 * @brief Main read implementation
 ******************************************************************************/

/*
 * Compute how many bytes to transfer from the current page position.
 *
 * Returns the minimum of:
 *   - bytes remaining in the page from lidx to dl_used(),
 *   - bytes remaining in the current read/skip window (bnext), and
 *   - if ACTIVE and max_bread > 0, the bytes remaining before the global
 *     read limit is reached.
 *
 * Skip-window bytes never count toward max_bread, so stride gaps are not
 * subject to the element count limit.
 */
static t_size
ns_read_next_amount (
    const page             *curp,
    const t_size            lidx,
    const b_size            bnext,
    const b_size            max_bread,
    const b_size            total_bread,
    const enum stride_phase state
)
{
  // Bytes available from the current page position.
  p_size next_amount = dl_used (curp) - lidx;

  // Capped by how far into this element/stride window we still need to
  // go.
  next_amount        = MIN (next_amount, bnext);

  // Capped by the global read limit (only in ACTIVE state; skip bytes do
  // not count toward max_bread).
  if (max_bread > 0 && state == ACTIVE) {
    next_amount = MIN (next_amount, max_bread - total_bread);
  }

  return next_amount;
}

// TODO - (4) tighten up the while loop to loop inside a page - rather than one
// read per loop
static sb_size
ns_read_forward (struct ns_read_params params, error *e)
{
  ASSERT (params.stride > 0);

  page_h                cur         = page_h_create ();
  page_h                next        = page_h_create ();
  p_size                lidx        = 0;
  b_size                total_bread = 0;
  b_size                max_bread   = params.size * params.nelem;
  b_size                bnext = params.size; // bytes remaining in the current read/skip window

  struct ns_seek_params seek  = {
      .p          = params.p,
      .tx         = params.tx,
      .root       = params.root,
      .bofst      = params.bofst,
      .save_stack = false,
      .sp         = 0,
  };

  enum stride_phase state = ACTIVE;

  // Nothing to read from an empty tree.
  if (params.root == PGNO_NULL) {
    return 0;
  }

  if (ns_seek (&seek, e)) {
    goto failed;
  }

  cur              = page_h_xfer_ownership (&seek.pg);
  lidx             = seek.lidx;

  const page *curp = page_h_ro (&cur);

  /*
  enum
  {
    HIT_MAX_READ,
    DEST_DONE_READING,
    DATA_EXHAUSTED,
  } termination = HIT_MAX_READ;
  */

  while (max_bread == 0 || total_bread < max_bread) {
    t_size next_amount = ns_read_next_amount (curp, lidx, bnext, max_bread, total_bread, state);

    if (next_amount == 0) {
      ASSERT (lidx <= dl_used (curp));
      if (lidx == dl_used (curp)) {
        pgno npg = dlgt_get_next (curp);

        if (npg == PGNO_NULL) {
          // termination = DATA_EXHAUSTED;
          break;
        }

        if (pgr_get (&next, PG_DATA_LIST, npg, params.p, e)) {
          goto failed;
        }

        if (pgr_release (params.p, &cur, PG_DATA_LIST, e)) {
          goto failed;
        }

        // Reset stuff
        lidx        = 0;
        cur         = page_h_xfer_ownership (&next);
        curp        = page_h_ro (&cur);
        next_amount = ns_read_next_amount (curp, lidx, bnext, max_bread, total_bread, state);

        ASSERT (next_amount > 0);
      } else {
        UNREACHABLE (); // LCOV_EXCL_LINE
      }
    }

    switch (state) {
      case ACTIVE: {
        sp_size
            read = stream_bwrite ((u8 *)dl_get_data (curp) + lidx, 1, next_amount, params.dest, e);

        if (read < 0) {
          goto failed;
        }

        lidx += (p_size)read;
        total_bread += (b_size)read;
        bnext -= (b_size)read;

        if (bnext == 0) {
          bnext = (b_size)(params.stride - 1) * params.size;
          state = SKIPPING;

          if (bnext == 0) {
            bnext = params.size;
            state = ACTIVE;
          }
        }
        break;
      }

      case SKIPPING: {
        p_size read = dl_read (curp, NULL, lidx, next_amount);
        lidx += read;
        bnext -= read;

        if (bnext == 0) {
          bnext = params.size;
          state = ACTIVE;
        }
        break;
      }
    }

    if (stream_isdone (params.dest)) {
      // termination = DEST_DONE_READING;
      break;
    }
  }

  // Release the final page.
  WRAP (pgr_release (params.p, &cur, page_get_type (curp), e));

  // Verify that we always stopped on a complete element boundary.
  if (total_bread % params.size != 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "read %" PRb_size
        " bytes, not a multiple of element size "
        "%" PRt_size,
        total_bread,
        params.size
    );
    goto failed;
  }

  return total_bread / params.size;

failed:
  pgr_cancel_if_exists (&cur);
  pgr_cancel_if_exists (&next);
  return error_trace (e);
}

sb_size
ns_rope_read (const struct ns_read_params params, error *e)
{
  if (params.stride > 0) {
    return ns_read_forward (params, e);
  }

  if (params.stride < 0) {
    return error_causef (e, ERR_INVALID_ARGUMENT, "Negative strides are not implemented (yet)");
  }

  return error_causef (e, ERR_INVALID_ARGUMENT, "read stride is 0");
}
