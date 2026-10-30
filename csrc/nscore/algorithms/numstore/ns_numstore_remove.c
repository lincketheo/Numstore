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
#include "core/ns_stream.h"
#include "core/ns_stride.h"
#include "core/ns_testing.h"
#include "nscore/algorithms/numstore/ns_numstore_algorithms.h"
#include "nscore/algorithms/rope/ns_rope_algorithms.h"
#include "nscore/algorithms/var/ns_var_algorithms.h"
#include "nscore/page/ns_page_fixture.h"
#include "nscore/types/ns_types.h"
#include "nscore/variables/ns_variables.h"

sb_size
numstore_remove_from_name (
    struct pager       *p,
    struct txn         *tx,
    struct string       name,
    struct user_stride  ustr,
    struct arena_alloc *valloc,
    struct variable    *var,
    struct stream      *dest,
    error              *e
)
{
  DBG_ASSERT (pager, p);
  DBG_ASSERT (ns_txn, tx);
  DBG_ASSERT (string, &name);
  DBG_ASSERT (user_stride, &ustr);
  DBG_ASSERT (arena_alloc, valloc);
  DBG_ASSERT_IF_NN (stream, dest);
  DBG_ASSERT (clean_error, e);

  // First, fetch the variable
  struct variable _var;
  if (var == NULL) {
    var = &_var;
  }
  if (numstore_get (p, tx, false, name, valloc, var, e) < 0) {
    return error_trace (e);
  }

  // Then, do the remove
  return numstore_remove (p, tx, var, ustr, dest, e);
}

sb_size
numstore_remove (
    struct pager      *p,
    struct txn        *tx,
    struct variable   *var,
    struct user_stride ustr,
    struct stream     *dest,
    error             *e
)
{
  DBG_ASSERT (pager, p);
  DBG_ASSERT (ns_txn, tx);
  DBG_ASSERT (variable, var);
  DBG_ASSERT (user_stride, &ustr);
  DBG_ASSERT_IF_NN (stream, dest);
  DBG_ASSERT (clean_error, e);

  // Resolve sizes
  t_size tsize = type_byte_size (var->dtype);
  ASSERT (var->nbytes % tsize == 0); // Variable is valid (from assert)
  b_size        len = var->nbytes / tsize;

  // Resolve length based on the stride
  struct stride stride;
  if (stride_resolve (&stride, ustr, len, e)) {
    return error_trace (e);
  }

  struct ns_remove_params rparams = {
      .p      = p,
      .dest   = dest,
      .tx     = tx,
      .root   = var->rpt_root,
      .size   = tsize,
      .bofst  = tsize * stride.start,
      .stride = stride.stride,
      .nelem  = stride.nelems,
  };
  sb_size ret = ns_remove (&rparams, e);
  if (ret < 0) {
    return error_trace (e);
  }

  // Update new sizes and root. ns_remove returns the number of elements it
  // took out, so the variable shrinks by that much - `len` is the length we
  // started with, not the amount removed.
  ASSERT ((b_size)ret * tsize <= var->nbytes);
  b_size newsize = var->nbytes - ((b_size)ret * tsize);
  pgno   newroot = rparams.root;

  if (ns_var_update_by_var_root (p, tx, var->var_root, newroot, newsize, e) < 0) {
    return error_trace (e);
  }

  // Update the user supplied variable
  var->nbytes   = newsize;
  var->rpt_root = rparams.root;

  return ret;
}

#ifndef NDEBUG

/// Seeds "var" with [0..9] as u32 so each case starts from the same 10
/// elements. Returns the variable as numstore_create left it.
static struct variable
remove_test_seed (struct pgr_fixture *f, struct test_ctx *ctx)
{
  struct variable var;
  numstore_create (f->p, &f->tx, strfcstr ("var"), TU32, &ctx->alloc, &var, &ctx->e);

  u32 data[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  istream_create_from_array (src, data);
  numstore_insert_from_name (
      f->p,
      &f->tx,
      strfcstr ("var"),
      0,
      10,
      &ctx->alloc,
      NULL,
      &src,
      &ctx->e
  );

  return var;
}

TEST (numstore_remove_from_name)
{
  TEST_CASE ("removes a slice, hands it back, and captures the variable")
  {
    struct pgr_fixture f;
    pgr_fixture_create (&f, &ctx->e);
    numstore_init_pager (f.p, &ctx->e);
    pgr_begin_txn (&f.tx, f.p, &ctx->e);

    const struct variable var        = remove_test_seed (&f, ctx);

    u32                   removed[3] = {0};
    ostream_create_from_array (out, removed);

    struct variable var_test;
    sb_size         ret = numstore_remove_from_name (
        f.p,
        &f.tx,
        strfcstr ("var"),
        ustride01 (2, 5), // elements 2, 3, 4
        &ctx->alloc,
        &var_test, // IMPORTANT - not null
        &out,
        &ctx->e
    );

    test_assert_int_equal (ret, 3);

    // The removed elements come out on [dest]
    const u32 expect_removed[3] = {2, 3, 4};
    test_assert_memequal (removed, expect_removed, sizeof (expect_removed));

    // The captured variable is the same variable, 3 elements lighter
    test_assert (variable_equal (
        &(struct variable){
            .nbytes   = 7 * sizeof (u32),
            .var_root = var.var_root,
            .vname    = var.vname,
            .dtype    = var.dtype,
            .rpt_root = var_test.rpt_root, // Might have changed
        },
        &var_test
    ));

    // And what is left is the slice's complement, in order
    u32 out_data[7] = {0};
    ostream_create_from_array (rest, out_data);
    sb_size   read        = numstore_read (f.p, &f.tx, &var_test, ustride0 (0), &rest, &ctx->e);

    const u32 expected[7] = {0, 1, 5, 6, 7, 8, 9};
    test_assert_int_equal (read, 7);
    test_assert_memequal (out_data, expected, sizeof (expected));

    pgr_commit (f.p, &f.tx, &ctx->e);
    pgr_fixture_teardown (&f, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("var and dest are both optional")
  {
    struct pgr_fixture f;
    pgr_fixture_create (&f, &ctx->e);
    numstore_init_pager (f.p, &ctx->e);
    pgr_begin_txn (&f.tx, f.p, &ctx->e);

    const struct variable var = remove_test_seed (&f, ctx);

    // Drop the first 4 elements on the floor - no output stream, no captured
    // variable, so the only way to see what happened is to look it up again
    sb_size               ret = numstore_remove_from_name (
        f.p,
        &f.tx,
        strfcstr ("var"),
        ustride1 (4), // elements 0..3
        &ctx->alloc,
        NULL, // IMPORTANT - null
        NULL, // IMPORTANT - null
        &ctx->e
    );

    test_assert_int_equal (ret, 4);

    struct variable var_test;
    test_assert_int_equal (
        numstore_get (f.p, &f.tx, false, strfcstr ("var"), &ctx->alloc, &var_test, &ctx->e),
        SUCCESS
    );

    test_assert (variable_equal (
        &(struct variable){
            .nbytes   = 6 * sizeof (u32),
            .var_root = var.var_root,
            .vname    = var.vname,
            .dtype    = var.dtype,
            .rpt_root = var_test.rpt_root,
        },
        &var_test
    ));

    u32 out_data[6] = {0};
    ostream_create_from_array (rest, out_data);
    sb_size   read        = numstore_read (f.p, &f.tx, &var_test, ustride0 (0), &rest, &ctx->e);

    const u32 expected[6] = {4, 5, 6, 7, 8, 9};
    test_assert_int_equal (read, 6);
    test_assert_memequal (out_data, expected, sizeof (expected));

    pgr_commit (f.p, &f.tx, &ctx->e);
    pgr_fixture_teardown (&f, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("a strided slice takes every other element")
  {
    struct pgr_fixture f;
    pgr_fixture_create (&f, &ctx->e);
    numstore_init_pager (f.p, &ctx->e);
    pgr_begin_txn (&f.tx, f.p, &ctx->e);

    remove_test_seed (&f, ctx);

    u32 removed[5] = {0};
    ostream_create_from_array (out, removed);

    struct variable var_test;
    sb_size         ret = numstore_remove_from_name (
        f.p,
        &f.tx,
        strfcstr ("var"),
        ustride2 (2), // [::2] - elements 0, 2, 4, 6, 8
        &ctx->alloc,
        &var_test,
        &out,
        &ctx->e
    );

    test_assert_int_equal (ret, 5);

    const u32 expect_removed[5] = {0, 2, 4, 6, 8};
    test_assert_memequal (removed, expect_removed, sizeof (expect_removed));
    test_assert_int_equal ((i32)var_test.nbytes, 5 * (i32)sizeof (u32));

    u32 out_data[5] = {0};
    ostream_create_from_array (rest, out_data);
    sb_size   read        = numstore_read (f.p, &f.tx, &var_test, ustride0 (0), &rest, &ctx->e);

    const u32 expected[5] = {1, 3, 5, 7, 9};
    test_assert_int_equal (read, 5);
    test_assert_memequal (out_data, expected, sizeof (expected));

    pgr_commit (f.p, &f.tx, &ctx->e);
    pgr_fixture_teardown (&f, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("an unknown name fails the lookup, not the remove")
  {
    struct pgr_fixture f;
    pgr_fixture_create (&f, &ctx->e);
    numstore_init_pager (f.p, &ctx->e);
    pgr_begin_txn (&f.tx, f.p, &ctx->e);

    remove_test_seed (&f, ctx);

    struct variable var_test;
    test_err_t_check (
        numstore_remove_from_name (
            f.p,
            &f.tx,
            strfcstr ("not-a-var"),
            USER_STRIDE_ALL,
            &ctx->alloc,
            &var_test,
            NULL,
            &ctx->e
        ),
        ERR_VARIABLE_NE,
        &ctx->e
    );

    // The variable that does exist is untouched
    struct variable var_real;
    test_assert_int_equal (
        numstore_get (f.p, &f.tx, false, strfcstr ("var"), &ctx->alloc, &var_real, &ctx->e),
        SUCCESS
    );
    test_assert_int_equal ((i32)var_real.nbytes, 10 * (i32)sizeof (u32));

    pgr_commit (f.p, &f.tx, &ctx->e);
    pgr_fixture_teardown (&f, &ctx->e);
    arena_alloc_free_all (&ctx->alloc);
  }
}

#endif
