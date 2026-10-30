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
 * SECTION: ns_rope_write
 * ----------------------------------------------------------------------------
 * @brief Overwrite data - don't insert
 ******************************************************************************/

/*
 * Compute how many bytes to overwrite starting at the current page position.
 *
 * Mirrors ns_read_next_amount exactly: minimum of bytes remaining on the
 * page from lidx, bytes remaining in the current write/skip window (bnext),
 * and â€” in ACTIVE state only â€” bytes remaining before the global write
 * limit (max_bwrite).  Skip-window bytes do not count toward max_bwrite.
 */
static t_size
ns_write_next_amount (
    const page             *curp,
    const t_size            lidx,
    const b_size            bnext,
    const b_size            max_bwrite,
    const b_size            total_bwrite,
    const enum stride_phase state
)
{
  // Available in the current page
  p_size next_amount = dl_used (curp) - lidx;

  // Available in this write state
  next_amount        = MIN (next_amount, bnext);

  // Available globally to write
  if (max_bwrite > 0 && state == ACTIVE) {
    next_amount = MIN (next_amount, max_bwrite - total_bwrite);
  }

  return next_amount;
}

// TODO - (4) tighten up the while loop to loop inside a page - rather than one
// read per loop
/*
 * Overwrite elements in the R+Tree with an optional stride, scanning forward.
 *
 * Structurally identical to ns_read_forward, but writes to the data-list
 * chain instead of reading from it.  The tree size does not change: write
 * overwrites existing bytes in place and stops at EOF rather than extending
 * the chain.  Requires pgr_make_writable() on each page before writing.
 *
 * ACTIVE state: pulls bytes from params->src and stamps them over the page
 * data starting at lidx.  SKIPPING state: advances lidx without reading from
 * src, leaving those bytes untouched.  When stride == 1 the SKIPPING phase
 * has zero bytes and is bypassed.
 *
 * Returns the number of complete elements written, not bytes.  Returns 0
 * immediately if root == PGNO_NULL (empty tree).
 */
static sb_size
ns_write_forward (const struct ns_write_params params, error *e)
{
  ASSERT (params.stride > 0);

  page_h                cur          = page_h_create ();
  page_h                next         = page_h_create ();
  p_size                lidx         = 0; // Local index on the current page
  b_size                total_bwrite = 0;
  const b_size          max_bwrite   = params.size * params.nelem;
  b_size                bnext        = params.size;

  struct ns_seek_params seek         = {
      .p          = params.p,
      .tx         = params.tx,
      .root       = params.root,
      .bofst      = params.bofst,
      .save_stack = false,
      .sp         = 0,
  };

  enum stride_phase state = ACTIVE;

  // Nothing to do
  if (params.root == PGNO_NULL) {
    return 0;
  }

  // Otherwise seek

  if (ns_seek (&seek, e)) {
    goto failed;
  }

  // Transition from Seeked -> inserting
  cur  = page_h_xfer_ownership (&seek.pg);
  lidx = seek.lidx;

  // Upgrade to X lock
  if (pgr_upgrade (&cur, params.tx, PG_DATA_LIST, params.p, e)) {
    goto failed;
  }

  page *curp = page_h_w (&cur);

  /*
  enum
  {
    HIT_MAX_WRITE,
    SRC_DONE_WRITING,
    DATA_EXHAUSTED,
  } termination = HIT_MAX_WRITE;
  */

  while (max_bwrite == 0 || total_bwrite < max_bwrite) {
    p_size next_amount = ns_write_next_amount (curp, lidx, bnext, max_bwrite, total_bwrite, state);

    if (next_amount == 0) {
      // Reached end of current page, advance
      // to next
      if (lidx >= dl_used (curp)) {
        const pgno npg = dlgt_get_next (curp);

        if (npg != PGNO_NULL) {
          WRAP (pgr_get_writable (&next, params.tx, PG_DATA_LIST, npg, params.p, e));
        }

        // Reached EOF
        else {
          // termination = DATA_EXHAUSTED;
          goto done;
        }

        WRAP (pgr_release (params.p, &cur, PG_DATA_LIST, e));

        lidx        = 0;
        cur         = page_h_xfer_ownership (&next);

        curp        = page_h_w (&cur);

        next_amount = ns_write_next_amount (curp, lidx, bnext, max_bwrite, total_bwrite, state);

        ASSERT (next_amount > 0);
      }

      else {
        UNREACHABLE (); // LCOV_EXCL_LINE
      }
    }

    switch (state) {
      case ACTIVE: {
        if (next_amount > 0) {
          // Pull bytes from caller's source stream and
          // stamp them into the page
          const sp_size
              write = stream_bread ((u8 *)dl_get_data (curp) + lidx, 1, next_amount, params.src, e);

          if (write < 0) {
            goto failed;
          }

          lidx += write;
          total_bwrite += write;
          bnext -= write;
        }

        if (bnext == 0) {
          // Finished writing one element; transition to
          // skip (stride-1) elements
          bnext = (params.stride - 1) * params.size;
          state = SKIPPING;

          // TODO - (5)
          // Optimize
          // stride = 1: skip window is zero, stay ACTIVE
          if (bnext == 0) {
            bnext = params.size;
            state = ACTIVE;
          }
        }
        break;
      }

      case SKIPPING: {
        if (next_amount > 0) {
          // Advance lidx without writing; NULL src means
          // leave bytes untouched
          const p_size write = dl_write (curp, NULL, lidx, next_amount);
          lidx += write;
          bnext -= write;
        }

        if (bnext == 0) {
          // Skip window exhausted; start writing the
          // next element
          bnext = params.size;
          state = ACTIVE;
        }
        break;
      }

      default: {
        UNREACHABLE (); // LCOV_EXCL_LINE
      }
    }

    // Caller's source exhausted before we hit the byte limit
    if (stream_isdone (params.src)) {
      // termination = SRC_DONE_WRITING;
      break;
    }
  }

done:

  // Release current page
  WRAP (pgr_release (params.p, &cur, PG_DATA_LIST, e));

  // Validate we write complete elements
  if (total_bwrite % params.size != 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "wrote %" PRb_size
        " bytes, not a multiple of element size "
        "%" PRt_size,
        total_bwrite,
        params.size
    );
    goto failed;
  }

  return total_bwrite / params.size;

failed:
  pgr_cancel_if_exists (&cur);
  pgr_cancel_if_exists (&next);
  return error_trace (e);
}

sb_size
ns_rope_write (const struct ns_write_params params, error *e)
{
  if (params.stride > 0) {
    return ns_write_forward (params, e);
  }
  if (params.stride < 0) {
    return error_causef (e, ERR_INVALID_ARGUMENT, "Negative strides are not implemented (yet)");
  } else {
    return error_causef (e, ERR_INVALID_ARGUMENT, "write stride is 0");
  }
}
