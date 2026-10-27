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
#include "core/ns_error.h"
#include "core/ns_stdtypes.h"
#include "core/ns_testing.h"
#include "nscore/algorithms/var/ns_var_algorithms.h"
#include "nscore/algorithms/var/ns_var_algorithms_internal.h"
#include "nscore/nsdb/ns_nsdb.h"
#include "nscore/page/ns_page.h"
#include "nscore/page/ns_page_h.h"
#include "nscore/page/ns_page_var_hash_page.h"
#include "nscore/page/ns_page_var_page.h"
#include "nscore/pager/ns_pager.h"
#include "nscore/types/ns_types.h"
#include "nscore/variables/ns_variables.h"

static err_t
ns_consume_one_var (struct pager *p, page_h *cur, var_consumer var, void *ctx, error *e)
{
  ALLOC_INIT (alloc);
  struct variable                v;

  // Read this one variable page
  struct ns_read_var_page_params params = {
      .p          = p,
      .tx         = NULL,

      .vp         = cur,
      .alloc      = &alloc,
      .dest       = &v,

      .matches    = false,
      .check      = NULL,

      .save_vname = true,
      .save_type  = true,
  };
  if (ns_read_var_page (&params, e) < 0) {
    goto theend;
  }

  // Execute the consumer
  if ((*var) (&v, ctx, e) < 0) {
    goto theend;
  }

theend:
  ALLOC_CLOSE (alloc);
  return error_trace (e);
}

static err_t
ns_consume_var_chain (struct pager *p, pgno start, var_consumer var, void *ctx, error *e)
{
  page_h cur = page_h_create ();

  err_t  ret = pgr_get (&cur, PG_VAR_PAGE, start, p, e);
  if (ret < 0) {
    goto failed;
  }

  while (true) {
    // Consume this page
    if (ns_consume_one_var (p, &cur, var, ctx, e) < 0) {
      goto failed;
    }

    // Maybe finish
    pgno next = vp_get_next (page_h_ro (&cur));

    // Release this page
    if (pgr_release (p, &cur, PG_VAR_PAGE, e) < 0) {
      goto failed;
    }

    if (next == PGNO_NULL) {
      return SUCCESS;
    }

    // Get the next page
    ret = pgr_get (&cur, PG_VAR_PAGE, next, p, e);
    if (ret < 0) {
      goto failed;
    }
  }

failed:
  pgr_cancel_if_exists (&cur);
  return error_trace (e);
}

err_t
ns_visit_variables (struct pager *p, var_consumer var, void *ctx, error *e)
{
  page_h vhp = page_h_create ();

  // Fetch the variable hash page
  err_t  ret = pgr_get (&vhp, PG_VAR_HASH_PAGE, VHASH_PGNO, p, e);
  if (ret < 0) {
    goto failed;
  }

  // Iterate through each root
  for (p_size i = 0; i < VH_HASH_LEN; ++i) {
    pgno pg = vh_get_hash_value (page_h_ro (&vhp), i);

    if (pg != PGNO_NULL) {
      if (ns_consume_var_chain (p, pg, var, ctx, e) < 0) {
        goto failed;
      }
    }
  }

  if (pgr_release (p, &vhp, PG_VAR_HASH_PAGE, e) < 0) {
    goto failed;
  }

  return SUCCESS;

failed:
  pgr_cancel_if_exists (&vhp);
  return error_trace (e);
}

#ifndef NDEBUG

struct visit_ctx
{
  u32    count;
  b_size total_nelems;
  t_size total_size;
  u32    varname_sum;
  u32    fail_on;
};

static err_t
test_visit_func (struct variable *var, void *ctx, error *e)
{
  if (var->vname.len != 4) {
    return error_causef (e, ERR_INVALID_ARGUMENT, "Failed test");
  }
  u32               v    = var->vname.data[3] - '0';

  struct visit_ctx *_ctx = ctx;
  _ctx->count += 1;
  _ctx->total_nelems += var->nbytes / type_byte_size (var->dtype);
  _ctx->total_size += type_byte_size (var->dtype);
  _ctx->varname_sum += v;

  if (v == _ctx->fail_on) {
    return error_causef (e, ERR_INVALID_ARGUMENT, "Intentional fail");
  }

  return SUCCESS;
}

TEST (ns_visit_variables)
{
  nsdb_cleanup ("test", ctx->os, &ctx->e);
  struct nsdb *db = nsdb_open ("test", ctx->mem, ctx->os, &ctx->e);
  nsdb_init_numstore (db, &ctx->e);

  TEST_CASE ("no variables")
  {
    struct visit_ctx vctx = {
        .count        = 0,
        .total_nelems = 0,
        .total_size   = 0,
        .varname_sum  = 0,
        .fail_on      = -1,
    };
    ns_visit_variables (db->p, test_visit_func, &vctx, &ctx->e);

    test_assert_int_equal (vctx.count, 0);
    test_assert_int_equal (vctx.total_nelems, 0);
    test_assert_int_equal (vctx.total_size, 0);
    test_assert_int_equal (vctx.varname_sum, 0);
  }

  TEST_CASE ("multiple variables")
  {
    struct txn *tx = nsdb_begin (db, &ctx->e);

    nsdb_execute (db, tx, "create var0 u32", &ctx->e);
    nsdb_execute (db, tx, "create var1 u32", &ctx->e);
    nsdb_execute (db, tx, "create var2 u32", &ctx->e);
    nsdb_execute (db, tx, "create var3 u32", &ctx->e);
    nsdb_execute (db, tx, "create var4 u32", &ctx->e);
    nsdb_execute (db, tx, "create var5 u32", &ctx->e);
    nsdb_execute (db, tx, "create var6 u32", &ctx->e);
    nsdb_execute (db, tx, "create var7 u32", &ctx->e);
    nsdb_execute (db, tx, "create var8 u32", &ctx->e);
    nsdb_execute (db, tx, "create var9 u32", &ctx->e);

    nsdb_commit (db, tx, &ctx->e);

    struct visit_ctx vctx = {
        .count        = 0,
        .total_nelems = 0,
        .total_size   = 0,
        .varname_sum  = 0,
        .fail_on      = -1,
    };
    ns_visit_variables (db->p, test_visit_func, &vctx, &ctx->e);

    test_assert_int_equal (vctx.count, 10);
    test_assert_int_equal (vctx.total_nelems, 0);
    test_assert_int_equal (vctx.total_size, 10 * sizeof (u32));
    test_assert_int_equal (vctx.varname_sum, 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9);
  }

  nsdb_close (db, &ctx->e);
}
#endif
