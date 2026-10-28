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
/**
 * @file
 * @brief Rope algorithms
 *
 * Various numstore rope algorithms
 */

#ifndef NS_ROPE_ALGORITHMS_H
#define NS_ROPE_ALGORITHMS_H

#include "core/ns_stream.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/pager/ns_pager.h"

enum stride_phase
{
  ACTIVE,
  SKIPPING,
};

struct ns_insert_params
{
  struct pager  *p;
  struct stream *src;
  struct txn    *tx;
  pgno           root;
  b_size         bofst;
  b_size         bytes;
};

struct ns_write_params
{
  struct pager  *p;
  struct stream *src;
  struct txn    *tx;
  pgno           root;
  t_size         size;
  b_size         bofst;
  sb_size        stride;
  b_size         nelem;
};

struct ns_read_params
{
  struct pager  *p;
  struct stream *dest;
  struct txn    *tx;
  pgno           root;
  t_size         size;
  b_size         bofst;
  sb_size        stride;
  b_size         nelem;
};

struct ns_remove_params
{
  struct pager  *p;
  struct stream *dest;
  struct txn    *tx;
  pgno           root;
  b_size         size;
  b_size         bofst;
  sb_size        stride;
  b_size         nelem;
};

err_t ns_insert (struct ns_insert_params *params, error *e);
sb_size ns_rope_write (struct ns_write_params params, error *e);
sb_size ns_rope_read (struct ns_read_params params, error *e);
sb_size ns_remove (struct ns_remove_params *params, error *e);

////////////////// Validate

err_t ns_rope_validate (struct pager *p, pgno root, error *e);

////////////////// Page statistics

struct rope_page_stats
{
  enum page_type type;
  pgno           pg;
  b_size         tracked;
  pgno           npg;
  pgno           ppg;
  bool           is_root;
  p_size         len;
  p_size         cap;
};

err_t ns_rope_page_stats (struct pager *p, struct rope_page_stats *dest, pgno pg, error *e);

////////////////// Layer Scan

/**
 * Scans each layer from the left most root
 */
i32 ns_rope_get_layers (struct pager *p, pgno dest[20], pgno root, error *e);

////////////////// IN Scan

typedef err_t (*in_page_consumer) (struct inner_node *in, void *ctx, error *e);

/**
 * Scans a inner node list dl FORWARD and executes [consumer] on each page it hits.
 * Note - if you supply a later page in the dl, you will not cover all pages
 */
err_t ns_rope_in_scan (
    struct pager    *p,
    const page_h    *cur,
    in_page_consumer consume,
    void            *ctx,
    error           *e
);

// Derrived from ns_rope_in_scan
err_t ns_rope_in_scan_pgno (
    struct pager    *p,
    pgno             pg,
    in_page_consumer consume,
    void            *ctx,
    error           *e
);
err_t ns_rope_in_validate_pgno (struct pager *p, pgno pg, error *e);

////////////////// DL Scan

typedef err_t (*page_consumer) (struct data_list dl, void *ctx, error *e);

/**
 * Scans a data list dl FORWARD and executes [consumer] on each page it hits.
 * Note - if you supply a later page in the dl, you will not cover all pages
 */
err_t ns_rope_dl_scan (
    struct pager *p,
    const page_h *cur,
    page_consumer consume,
    void         *ctx,
    error        *e
);
err_t ns_rope_dl_scan_pgno (struct pager *p, pgno pg, page_consumer consume, void *ctx, error *e);

// Derrived from rope dl scan
sb_size ns_rope_dl_scan_blen (struct pager *p, page_h *cur, error *e);
sb_size ns_rope_dl_scan_blen_pgno (struct pager *p, pgno pg, error *e);
sb_size ns_rope_dl_scan_npages (struct pager *p, page_h *cur, error *e);

////////////////// Chain Initialization

typedef err_t (*page_initializer) (page *cur, void *ctx, bool *done, error *e);

/**
 * Creates a new page (returns that page)
 */
spgno ns_rope_dl_init (struct pager *p, struct txn *tx, page_initializer init, void *ctx, error *e);

// Derrived from dl_init
spgno ns_rope_dl_init_from (struct pager *p, void *data, b_size dlen, error *e);

#endif // NS_ROPE_ALGORITHMS_H
