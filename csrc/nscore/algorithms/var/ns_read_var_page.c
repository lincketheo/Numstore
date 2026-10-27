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

#include "core/ns_arena_alloc.h"
#include "core/ns_bytes.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_serial.h"
#include "core/ns_stdtypes.h"
#include "core/ns_string.h"
#include "core/ns_testing.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"
#include "nscore/algorithms/var/ns_var_algorithms_internal.h"
#include "nscore/nsdb/ns_nsdb.h"
#include "nscore/page/ns_page.h"
#include "nscore/page/ns_page_delegate.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/page/ns_page_var_page.h"
#include "nscore/pager/ns_pager.h"
#include "nscore/types/ns_types.h"
#include "nscore/variables/ns_variables.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Advance params->vp from the current PG_VAR_PAGE or PG_VAR_TAIL to the
 * next overflow page in the chain.  Used when a variable's serialised name
 * and type data spans more than one page.
 */
static err_t
ns_read_var_page_advance (struct ns_read_var_page_params *params, error *e)
{
  page_h next = page_h_create ();

  pgno   npg  = dlgt_get_ovnext (page_h_ro (params->vp));

  if (npg == PGNO_NULL) {
    error_causef (e, ERR_CORRUPT, "var page missing overflow pointer");
    goto failed;
  }

  WRAP (pgr_get_writable (&next, params->tx, PG_VAR_TAIL, npg, params->p, e));

  if (pgr_release (params->p, params->vp, PG_VAR_PAGE | PG_VAR_TAIL, e)) {
    goto failed;
  }

  page_h_xfer_ownership_ptr (params->vp, &next);

  return SUCCESS;

failed:
  return error_trace (e);
}

err_t
ns_read_var_page (struct ns_read_var_page_params *params, error *e)
{
  ASSERT (params->vp->mode != PHM_NONE);
  ASSERT (page_h_type (params->vp) == PG_VAR_PAGE);

  // Save for the end to reset var page
  pgno start    = page_h_pgno (params->vp);
  bool writable = params->vp->mode == PHM_X;
  ASSERT (!writable || params->tx);

  // Temporary allocator
  ALLOC_INIT (temp);

  // Initialize the simple stuff
  u16    vlen     = vp_get_vlen (page_h_ro (params->vp));
  u16    tlen     = vp_get_tlen (page_h_ro (params->vp));
  pgno   rpt_root = vp_get_root (page_h_ro (params->vp));
  b_size nbytes   = vp_get_nbytes (page_h_ro (params->vp));
  pgno   var_root = page_h_pgno (params->vp);

  if (params->dest) {
    params->dest->rpt_root = rpt_root;
    params->dest->nbytes   = nbytes;
    params->dest->var_root = var_root;
  }

  // Quick check on the length
  if ((params->check) && (vlen != params->check->len)) {
    params->matches = false;
    goto theend;
  }

  // This is the "scanning string" if save_vname is true, it'll
  // be memcpy'ed at the end to the persistent params->alloc
  // It is one extra memcpy - but if you allocate this on
  // persistent, then the all variable names in the chain
  // will live on the passed memory, but it'd be a tiny
  // bit faster, but not worth the speed up
  //
  // TODO - in the future, an arena_malloc_commit
  // would be good for this - allowing the arena allocator
  // have a rollback
  char *vstr = arena_malloc (&temp, 1, vlen, e);
  if (vstr == NULL) {
    goto failed;
  }

  // This is the type string, it's only ever lives on
  // the temporary allocator and it's only ever used
  // if save type is true
  u8 *tstr = NULL;
  if (params->save_type) {
    tstr = arena_malloc (&temp, 1, tlen, e);
    if (tstr == NULL) {
      goto failed;
    }
  }

  // Read the variable name
  u16           total_read = 0; // Total read for the whole variable name
  p_size        local_read = 0; // Just the number of bytes read on each page
  struct cbytes head       = dlgt_get_bytes_imut (page_h_ro (params->vp));
  while (total_read < vlen) {
    // We exhausted this page - move forward one
    if (local_read == head.len) {
      if (ns_read_var_page_advance (params, e)) {
        goto failed;
      }

      local_read = 0;
      head       = dlgt_get_bytes_imut (page_h_ro (params->vp));
    }

    // Do the read of the name
    u16 toread = vlen - total_read;
    u16 bavail = head.len - local_read;
    u16 next   = MIN (bavail, toread);

    memcpy (&vstr[total_read], &head.head[local_read], next);
    total_read += next;
    local_read += next;
  }

  // Quick termination on string data
  if (params->check && (memcmp (params->check->data, vstr, vlen) != 0)) {
    params->matches = false;
    goto theend;
  }

  // Only execute this next block if we want to save the
  // type. This is because the type is tail data
  if (params->save_type) {
    // Read the type bytes
    total_read = 0;
    while (total_read < tlen) {
      // We exhausted this page - move forward one
      if (local_read == head.len) {
        // Advance forward one node and reset local and head
        if (ns_read_var_page_advance (params, e)) {
          goto failed;
        }
        local_read = 0;
        head       = dlgt_get_bytes_imut (page_h_ro (params->vp));
      }

      // MIN(available in this node, available to be read left)
      u16 avail = head.len - local_read;
      u16 left  = tlen - total_read;
      u16 next  = MIN (avail, left);

      // Do the read
      memcpy (&tstr[total_read], &head.head[local_read], next);
      total_read += next;
      local_read += next;
    }

    // This check ensures that this page is truly the last
    // page of the chain. The last page should point next
    // to NULL, if it doesn't then it's an invalid variable
    if (dlgt_get_ovnext (page_h_ro (params->vp)) != PGNO_NULL) {
      error_causef (
          e,
          ERR_CORRUPT,
          "var page read complete but overflow "
          "pointer is non-null"
      );
      goto failed;
    }
  }

  // Set the values

  if (params->save_type) {
    // Copy the type onto the persistent allocator
    struct deserializer d     = dsrlizr_create (tstr, tlen);
    struct type        *dtype = type_deserialize (&d, params->alloc, e);
    if (dtype == NULL) {
      goto failed;
    }
    params->dest->dtype = dtype;
  }

  if (params->save_vname) {
    char *vstr_permanent = arena_malloc (params->alloc, vlen, 1, e);
    if (vstr_permanent == NULL) {
      goto failed;
    }
    memcpy (vstr_permanent, vstr, vlen);
    params->dest->vname = (struct string){.data = vstr_permanent, .len = vlen};
  }

  params->matches = true;

theend:
  // Reset back to head page
  if (page_h_pgno (params->vp) != start) {
    if (pgr_release (params->p, params->vp, PG_VAR_TAIL, e)) {
      goto failed;
    }
    if (pgr_get_maybe_writable (
            params->vp,
            params->tx,
            PG_VAR_PAGE,
            start,
            params->p,
            writable,
            e
        )) {
      goto failed;
    }
  }

  ALLOC_CLOSE (temp);
  return SUCCESS;

failed:
  ALLOC_CLOSE (temp);
  return error_trace (e);
}

#ifndef NDEBUG

// Must be big enough that the name spills past the first PG_VAR_PAGE
// into at least one PG_VAR_TAIL (and must fit in a u16)
#  define LONG_VNAME_LEN (3 * NS_PAGE_SIZE)
_Static_assert (LONG_VNAME_LEN == (u32)(3 * NS_PAGE_SIZE), "u16 overflow");

static inline void
fill_long_name (char *buf, size_t len, char tag)
{
  memset (buf, 'n', len);
  buf[len - 1] = tag; // only the last byte differs between names
  buf[len]     = '\0';
}

static inline void
create_and_get_root (
    page_h      *dest,
    bool         writable,
    struct nsdb *db,
    struct txn  *tx,
    const char  *name,
    struct i_mem mem,
    error       *e
)
{
  // Build the query
  int   n     = snprintf (NULL, 0, "create %s u32", name);
  char *query = mem.table->malloc (mem.self, n + 1, 1, e);

  // Create the variable
  snprintf (query, n + 1, "create %s u32", name);
  nsdb_execute (db, tx, query, e);

  // Get the variable
  snprintf (query, n + 1, "get %s", name);
  struct nsdb_var *var = nsdb_get_var (db, tx, query, e);
  mem.table->free (mem.self, query);

  // Get the root
  pgno root = var->var.var_root;
  nsdb_var_free (var);

  if (writable) {
    pgr_get_writable (dest, tx, PG_VAR_PAGE, root, db->p, e);
  } else {
    pgr_get (dest, PG_VAR_PAGE, root, db->p, e);
  }
}

TEST (ns_read_var_page)
{
  /* Create a new database */
  nsdb_cleanup ("test", ctx->os, &ctx->e);
  struct nsdb *db = nsdb_open ("test", ctx->mem, ctx->os, &ctx->e);
  nsdb_init_numstore (db, &ctx->e);

  struct txn *tx = nsdb_begin (db, &ctx->e);

  /* Long names - every test creates its own variable, so each needs a unique
   * name */
  char        long_a[LONG_VNAME_LEN + 1];
  fill_long_name (long_a, LONG_VNAME_LEN, 'a');
  char long_b[LONG_VNAME_LEN + 1];
  fill_long_name (long_b, LONG_VNAME_LEN, 'b');
  char long_c[LONG_VNAME_LEN + 1];
  fill_long_name (long_c, LONG_VNAME_LEN, 'c');
  char long_d[LONG_VNAME_LEN + 1];
  fill_long_name (long_d, LONG_VNAME_LEN, 'd');
  char long_e[LONG_VNAME_LEN + 1];
  fill_long_name (long_e, LONG_VNAME_LEN, 'e');
  char long_f[LONG_VNAME_LEN + 1];
  fill_long_name (long_f, LONG_VNAME_LEN, 'f');
  char long_y[LONG_VNAME_LEN + 1];
  fill_long_name (long_y, LONG_VNAME_LEN, 'y'); // never created
  char long_z[LONG_VNAME_LEN + 1];
  fill_long_name (long_z, LONG_VNAME_LEN, 'z'); // never created

  TEST_CASE ("Handle stays in X mode if it starts in X mode")
  {
    page_h cur = page_h_create ();
    create_and_get_root (
        &cur,
        true, // IMPORTANT
        db,
        tx,
        "mode_x_plain",
        ctx->mem,
        &ctx->e
    );

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL,
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_X); // IMPORTANT

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data == NULL); // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Handle stays in S mode if it starts in S mode")
  {
    page_h cur = page_h_create ();
    create_and_get_root (
        &cur,
        false, // IMPORTANT
        db,
        tx,
        "mode_s_plain",
        ctx->mem,
        &ctx->e
    );

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL,
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S); // IMPORTANT

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data == NULL); // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Handle stays in X mode with save_vname and save_type")
  {
    page_h cur = page_h_create ();
    create_and_get_root (
        &cur,
        true, // IMPORTANT
        db,
        tx,
        "mode_x_saves",
        ctx->mem,
        &ctx->e
    );

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL,
        .save_vname = true,
        .save_type  = true,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_X); // IMPORTANT

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data != NULL); // save_vname == true, matches == true
    test_assert_int_equal (var.vname.len, strlen ("mode_x_saves"));
    test_assert (memcmp (var.vname.data, "mode_x_saves", strlen ("mode_x_saves")) == 0);
    test_assert (var.dtype != NULL); // save_type == true, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Handle stays in X mode when vname doesn't match")
  {
    page_h cur = page_h_create ();
    create_and_get_root (
        &cur,
        true, // IMPORTANT
        db,
        tx,
        "mode_x_nomatch",
        ctx->mem,
        &ctx->e
    );

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("mode_x_nomatcX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check,
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_X); // IMPORTANT

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false);
    test_assert (var.vname.data == NULL); // save_vname == false, matches == false
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == false
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Matches is true when check is NULL")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "match_null", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data == NULL);         // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Matches is true when vname matches")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "match_eq", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("match_eq");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data == NULL);         // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Matches is false when vname differs in content (same length)")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "match_diff", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("match_difX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // save_vname == false, matches == false
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == false
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Matches is false when check is shorter than vname")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "match_short", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("match_shor");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // save_vname == false, matches == false
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == false
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Matches is false when check is longer than vname")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "match_long", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("match_longX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // save_vname == false, matches == false
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == false
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("vname is saved when save_vname and check is NULL")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "sv_null", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL, // IMPORTANT
        .save_vname = true, // IMPORTANT
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data != NULL);         // IMPORTANT (save_vname == true, matches == true)
    test_assert_int_equal (var.vname.len, strlen ("sv_null"));
    test_assert (memcmp (var.vname.data, "sv_null", strlen ("sv_null")) == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("vname is saved when save_vname and vname matches")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "sv_eq", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("sv_eq");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = &check, // IMPORTANT
        .save_vname = true,   // IMPORTANT
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data != NULL);         // IMPORTANT (save_vname == true, matches == true)
    test_assert_int_equal (var.vname.len, strlen ("sv_eq"));
    test_assert (memcmp (var.vname.data, "sv_eq", strlen ("sv_eq")) == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("vname isn't saved when save_vname and vname content differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "sv_diff", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("sv_difX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = true,   // IMPORTANT
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL); // IMPORTANT (save_vname == true, matches == false)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == false
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("vname isn't saved when save_vname and vname length differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "sv_len", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("sv_lenXX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = true,   // IMPORTANT
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL); // IMPORTANT (save_vname == true, matches == false)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == false
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("dtype is saved (vname isn't) when save_type and check is NULL")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "st_null", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL, // IMPORTANT
        .save_vname = false,
        .save_type  = true, // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data == NULL);         // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype != NULL); // IMPORTANT (save_type == true, matches == true)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("dtype is saved (vname isn't) when save_type and vname matches")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "st_eq", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("st_eq");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = true, // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data == NULL);         // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype != NULL); // IMPORTANT (save_type == true, matches == true)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("dtype isn't saved when save_type and vname content differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "st_diff", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("st_difX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = true, // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // save_vname == false, matches == false
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (save_type == true, matches == false)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("dtype isn't saved when save_type and vname length differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "st_len", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("st_lenXX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = true, // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // save_vname == false, matches == false
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (save_type == true, matches == false)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("vname and dtype are saved when both flags set and check is NULL")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "both_null", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL, // IMPORTANT
        .save_vname = true, // IMPORTANT
        .save_type  = true, // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data != NULL);         // IMPORTANT (save_vname == true, matches == true)
    test_assert_int_equal (var.vname.len, strlen ("both_null"));
    test_assert (memcmp (var.vname.data, "both_null", strlen ("both_null")) == 0);
    test_assert (var.dtype != NULL); // IMPORTANT (save_type == true, matches == true)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("vname and dtype are saved when both flags set and vname matches")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "both_eq", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("both_eq");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = &check, // IMPORTANT
        .save_vname = true,   // IMPORTANT
        .save_type  = true,   // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data != NULL);         // IMPORTANT (save_vname == true, matches == true)
    test_assert_int_equal (var.vname.len, strlen ("both_eq"));
    test_assert (memcmp (var.vname.data, "both_eq", strlen ("both_eq")) == 0);
    test_assert (var.dtype != NULL); // IMPORTANT (save_type == true, matches == true)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Neither is saved when both flags set and vname content differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "both_diff", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("both_difX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = true,   // IMPORTANT
        .save_type  = true,   // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL); // IMPORTANT (save_vname == true, matches == false)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (save_type == true, matches == false)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Neither is saved when both flags set and vname length differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "both_len", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("both_lenXX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = true,   // IMPORTANT
        .save_type  = true,   // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL); // IMPORTANT (save_vname == true, matches == false)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (save_type == true, matches == false)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Long vname: handle returns to the root var page")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, long_a, ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL,
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur));
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data == NULL); // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Long vname: saved in full when save_vname")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, long_b, ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL,
        .save_vname = true, // IMPORTANT
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur)); // IMPORTANT
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data != NULL); // IMPORTANT (save_vname == true, matches == true)
    test_assert_int_equal (var.vname.len, strlen (long_b));
    test_assert (memcmp (var.vname.data, long_b, strlen (long_b)) == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Long vname: X mode handle returns to root with both flags set")
  {
    page_h cur = page_h_create ();
    create_and_get_root (
        &cur,
        true, // IMPORTANT
        db,
        tx,
        long_c,
        ctx->mem,
        &ctx->e
    );

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = NULL,
        .save_vname = true, // IMPORTANT
        .save_type  = true, // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_X); // IMPORTANT

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur)); // IMPORTANT
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data != NULL); // IMPORTANT (save_vname == true, matches == true)
    test_assert_int_equal (var.vname.len, strlen (long_c));
    test_assert (memcmp (var.vname.data, long_c, strlen (long_c)) == 0);
    test_assert (var.dtype != NULL); // IMPORTANT (save_type == true, matches == true)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Long vname: matches when check is equal")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, long_d, ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr (long_d);
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = false,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur)); // IMPORTANT
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data == NULL);         // save_vname == false, matches == true
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == true
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Long vname: doesn't match when only the last byte differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, long_e, ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr (long_z);
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur)); // IMPORTANT
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // save_vname == false, matches == false
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // save_type == false, matches == false
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Long vname: nothing saved when last byte differs and both flags set")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, long_f, ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr (long_y);
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = &var,
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = true,   // IMPORTANT
        .save_type  = true,   // IMPORTANT
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert_int_equal (var.var_root, page_h_pgno (&cur)); // IMPORTANT
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL); // IMPORTANT (save_vname == true, matches == false)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (save_type == true, matches == false)
    test_assert (var.rpt_root == PGNO_NULL);
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  /* Add next to the other long_* declarations */
  char long_g[LONG_VNAME_LEN + 1];
  fill_long_name (long_g, LONG_VNAME_LEN, 'g');
  char long_h[LONG_VNAME_LEN + 1];
  fill_long_name (long_h, LONG_VNAME_LEN, 'h');
  char long_x[LONG_VNAME_LEN + 1];
  fill_long_name (long_x, LONG_VNAME_LEN, 'x'); // never created

  TEST_CASE ("NULL dest: succeeds with no check (S mode)")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "dn_plain", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = NULL, // IMPORTANT
        .matches    = false,
        .check      = NULL,
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert (var.var_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.rpt_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("NULL dest: succeeds with no check (X mode)")
  {
    page_h cur = page_h_create ();
    create_and_get_root (
        &cur,
        true, // IMPORTANT
        db,
        tx,
        "dn_x",
        ctx->mem,
        &ctx->e
    );

    struct variable                var    = {0};
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = NULL, // IMPORTANT
        .matches    = false,
        .check      = NULL,
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_X); // IMPORTANT

    // Handle is back on the root var page
    test_assert (var.var_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true);
    test_assert (var.vname.data == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.rpt_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("NULL dest: matches is true when vname matches")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "dn_eq", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("dn_eq");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = NULL, // IMPORTANT
        .matches    = false,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert (var.var_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data == NULL);         // IMPORTANT (dest == NULL, untouched)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.rpt_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("NULL dest: matches is false when vname content differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "dn_diff", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("dn_difX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = NULL, // IMPORTANT
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert (var.var_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // IMPORTANT (dest == NULL, untouched)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.rpt_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("NULL dest: matches is false when vname length differs")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, "dn_len", ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr ("dn_lenXX");
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = NULL, // IMPORTANT
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert (var.var_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // IMPORTANT (dest == NULL, untouched)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.rpt_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("NULL dest: long vname matches and handle returns to root")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, long_g, ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr (long_g);
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = NULL, // IMPORTANT
        .matches    = false,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert (var.var_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, true); // IMPORTANT
    test_assert (var.vname.data == NULL);         // IMPORTANT (dest == NULL, untouched)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.rpt_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("NULL dest: long vname doesn't match on last byte")
  {
    page_h cur = page_h_create ();
    create_and_get_root (&cur, false, db, tx, long_h, ctx->mem, &ctx->e);

    struct variable                var    = {0};
    struct string                  check  = strfcstr (long_x);
    struct ns_read_var_page_params params = {
        .p          = db->p,
        .tx         = tx,

        .vp         = &cur,
        .alloc      = &ctx->alloc,

        .dest       = NULL, // IMPORTANT
        .matches    = true,
        .check      = &check, // IMPORTANT
        .save_vname = false,
        .save_type  = false,
    };
    test_assert_int_equal (ns_read_var_page (&params, &ctx->e), SUCCESS);

    // Variable is in the same mode as before
    test_assert (cur.mode == PHM_S);

    // Handle is back on the root var page
    test_assert (var.var_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert (page_h_type (&cur) == PG_VAR_PAGE);

    test_assert_int_equal (params.matches, false); // IMPORTANT
    test_assert (var.vname.data == NULL);          // IMPORTANT (dest == NULL, untouched)
    test_assert (var.vname.len == 0);
    test_assert (var.dtype == NULL); // IMPORTANT (dest == NULL, untouched)
    test_assert (var.rpt_root == 0); // IMPORTANT (dest == NULL, untouched)
    test_assert_int_equal (var.nbytes, 0);

    pgr_release (db->p, &cur, PG_VAR_PAGE, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  nsdb_commit (db, tx, &ctx->e);
  nsdb_close (db, &ctx->e);
}

#endif
