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

#include "nscore/testing/simulation/ns_ref_state_machine.h"

#include "core/ns_arena_alloc.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_ext_array.h"
#include "core/ns_string.h"
#include "core/ns_testing.h"
#include "core/os/ns_malloc.h"
#include "nscore/compiler/ns_compiler.h"
#include "nscore/testing/simulation/ns_mem_vhmap.h"
#include "nscore/types/ns_types.h"

#include <string.h>

static inline struct db_state *
ns_ref_cur (struct ns_ref *ref)
{
  if (ref->in_txn) {
    return ref->working;
  } else {
    return ref->committed;
  }
}

// DB STATE

static struct db_state *
db_state_create (struct i_mem mem, error *e)
{
  struct db_state *ret = mem.table->malloc (mem.self, 1, sizeof *ret, e);
  if (ret == NULL) {
    return NULL;
  }

  ret->db_data = mem_vhmap_create (mem, e);
  if (ret->db_data == NULL) {
    mem.table->free (mem.self, ret);
    return NULL;
  }

  ret->cur           = NULL;
  ret->nvars         = 0;
  ret->tracked_bytes = 0;
  ret->mem           = mem;

  return ret;
}

static void
db_state_free (struct db_state *db)
{
  mem_vhmap_free (db->db_data);
  struct i_mem mem = db->mem;
  mem.table->free (mem.self, db);
}

struct db_state *
db_state_clone (struct i_mem mem, struct db_state *state, error *e)
{
  struct db_state *ret = db_state_create (mem, e);
  if (ret == NULL) {
    return NULL;
  }

  // Clone db_data
  struct mem_vhmap *cloned_db_data = mem_vhmap_clone (mem, state->db_data, e);
  if (cloned_db_data == NULL) {
    db_state_free (ret);
    return NULL;
  }

  // Clone variable name
  struct var_with_data *new_cur = NULL;
  if (state->cur) {
    new_cur = mem_vhmap_get (cloned_db_data, state->cur->var.vname);
    if (new_cur == NULL) {
      mem_vhmap_free (cloned_db_data);
      db_state_free (ret);
      return NULL;
    }
  }

  // Free the one that was given to us by create
  mem_vhmap_free (ret->db_data);

  ret->db_data       = cloned_db_data;
  ret->cur           = new_cur;
  ret->nvars         = state->nvars;
  ret->tracked_bytes = state->tracked_bytes;
  ret->mem           = mem;

  return ret;
}

// NS_REF

struct ns_ref *
ns_ref_new (struct i_mem mem, error *e)
{
  struct ns_ref *ref = mem.table->malloc (mem.self, 1, sizeof *ref, e);
  if (ref == NULL) {
    return NULL;
  }

  struct db_state *committed = db_state_create (mem, e);
  if (committed == NULL) {
    mem.table->free (mem.self, ref);
    return NULL;
  }

  *ref = (struct ns_ref){
      .committed = committed,
      .working   = NULL,
      .mem       = mem,
      .in_txn    = false,
  };

  return ref;
}

void
ns_ref_free (struct ns_ref *ref)
{
  if (ref->working) {
    db_state_free (ref->working);
  }
  if (ref->committed) {
    db_state_free (ref->committed);
  }
  ref->mem.table->free (ref->mem.self, ref);
}

// Return nu
u32
ns_ref_nvars (struct ns_ref *ref)
{
  return ns_ref_cur (ref)->nvars;
}

u32
ns_ref_tracked_bytes (struct ns_ref *ref)
{
  return ns_ref_cur (ref)->tracked_bytes;
}

const char *
ns_ref_cur_name (struct ns_ref *ref)
{
  struct db_state *state = ns_ref_cur (ref);
  ASSERT (state->cur);
  return state->cur->var.vname.data;
}

b_size
ns_ref_cur_len (struct ns_ref *ref)
{
  struct db_state *state = ns_ref_cur (ref);
  ASSERT (state->cur);
  t_size size = type_byte_size (state->cur->var.dtype);
  return ext_array_get_len (&state->cur->data) / size;
}

t_size
ns_ref_cur_tsize (struct ns_ref *ref)
{
  struct db_state *state = ns_ref_cur (ref);
  ASSERT (state->cur);
  return type_byte_size (state->cur->var.dtype);
}

bool
ns_ref_var_exists (struct ns_ref *ref, const char *name)
{
  return ns_ref_var_exists_str (ref, strfcstr (name));
}

bool
ns_ref_var_exists_str (struct ns_ref *ref, struct string name)
{
  struct db_state *state = ns_ref_cur (ref);
  return mem_vhmap_get (state->db_data, name) != NULL;
}

const char *
ns_ref_random_var (struct ns_ref *ref)
{
  struct db_state *state = ns_ref_cur (ref);
  return mem_vhmap_random (state->db_data)->var.vname.data;
}

err_t
ns_ref_begin_txn (struct ns_ref *ref, error *e)
{
  ASSERT (ref);
  ASSERT (!ref->in_txn);
  ASSERT (ref->working == NULL);
  ASSERT (ref->committed);

  ref->working = db_state_clone (ref->mem, ref->committed, e);

  if (ref->working == NULL) {
    return error_trace (e);
  }

  ref->in_txn = true;

  return SUCCESS;
}

void
ns_ref_rollback_txn (struct ns_ref *ref)
{
  ASSERT (ref);
  ASSERT (ref->in_txn);
  ASSERT (ref->working);
  ASSERT (ref->committed);

  db_state_free (ref->working);
  ref->working = NULL;
  ref->in_txn  = false;
}

err_t
ns_ref_commit_txn (struct ns_ref *ref, error *e)
{
  ASSERT (ref);
  ASSERT (ref->in_txn);
  ASSERT (ref->working);
  ASSERT (ref->committed);

  struct db_state *new_committed = db_state_clone (ref->mem, ref->working, e);
  if (new_committed == NULL) {
    return error_trace (e);
  }

  db_state_free (ref->committed);
  db_state_free (ref->working);

  ref->committed = new_committed;
  ref->working   = NULL;
  ref->in_txn    = false;

  return SUCCESS;
}

void
ns_ref_crash_and_reopen (struct ns_ref *ref)
{
  if (ref->in_txn) {
    ns_ref_rollback_txn (ref);
  }
}

void
ns_ref_close_and_reopen (struct ns_ref *ref)
{
  ASSERT (!ref->in_txn);
  (void)ref;
}

err_t
ns_ref_create (struct ns_ref *ref, const char *vname, struct type *type, error *e)
{
  struct db_state *state = ns_ref_cur (ref);

  struct variable  var   = {
      .vname    = strfcstr (vname),
      .dtype    = type,
      .nbytes   = 0,
      .rpt_root = 0,
      .var_root = 0,
  };
  struct var_with_data *data = mem_vhmap_add (state->db_data, &var, e);

  if (data == NULL) {
    return error_trace (e);
  }

  state->nvars += 1;

  if (state->cur == NULL) {
    state->cur = data;
  }

  return SUCCESS;
}

void
ns_ref_switch (struct ns_ref *ref, const char *next)
{
  struct var_with_data *data = mem_vhmap_get (ns_ref_cur (ref)->db_data, strfcstr (next));

  // Shouldn't fail - provide a valid variable name
  ASSERT (data);

  ns_ref_cur (ref)->cur = data;
}

void
ns_ref_delete_and_switch (struct ns_ref *ref, const char *next)
{
  struct db_state      *state = ns_ref_cur (ref);
  struct var_with_data *data  = NULL;
  if (next != NULL) {
    data = mem_vhmap_get (state->db_data, strfcstr (next));
  }

  // Shouldn't pass the same variable
  ASSERT (state->cur);
  ASSERT (data != state->cur);
  ASSERT (state->nvars > 0);

  u64 len = ext_array_get_len (&state->cur->data);

  mem_vhmap_remove (state->db_data, state->cur->var.vname);
  state->cur = data;
  state->nvars -= 1;
  state->tracked_bytes -= len;
}

sb_size
ns_ref_insert (struct ns_ref *ref, void *data, b_size ofst, b_size len, error *e)
{
  struct db_state *state = ns_ref_cur (ref);
  ASSERT (state->cur);

  t_size size     = type_byte_size (state->cur->var.dtype);
  i64    inserted = ext_array_insert (&state->cur->data, ofst * size, data, len * size, e);
  if (inserted < 0) {
    return error_trace (e);
  }

  state->tracked_bytes += inserted;

  return inserted / size;
}

b_size
ns_ref_remove (struct ns_ref *ref, void *dest, struct stride str)
{
  struct db_state      *state = ns_ref_cur (ref);
  struct var_with_data *cur   = state->cur;
  ASSERT (cur);

  t_size size    = type_byte_size (cur->var.dtype);
  u64    removed = ext_array_remove (&cur->data, str, size, dest);
  state->tracked_bytes -= removed * size;

  return removed;
}

b_size
ns_ref_read (struct ns_ref *ref, void *dest, struct stride str)
{
  struct var_with_data *cur = ns_ref_cur (ref)->cur;
  ASSERT (cur);
  return ext_array_read (&cur->data, str, type_byte_size (cur->var.dtype), dest);
}

b_size
ns_ref_write (struct ns_ref *ref, void *data, struct stride str)
{
  struct var_with_data *cur = ns_ref_cur (ref)->cur;
  ASSERT (cur);
  return ext_array_write (&cur->data, str, type_byte_size (cur->var.dtype), data);
}

#ifndef NDEBUG

TEST (ns_ref)
{
  struct ns_ref *ref = ns_ref_new (ctx->mem, &ctx->e);

  u32            dest[20];

#  define validate(expected)                                                           \
    do {                                                                               \
      ns_ref_read (ref, dest, (struct stride){.start = 0, .stride = 1, .nelems = 20}); \
      test_assert_memequal (expected, dest, sizeof (expected));                        \
    }                                                                                  \
    while (0)

#  define check_state(_nvars, _tracked_bytes)                                  \
    do {                                                                       \
      test_assert_int_equal (ns_ref_cur (ref)->nvars, _nvars);                 \
      test_assert_int_equal (ns_ref_cur (ref)->tracked_bytes, _tracked_bytes); \
    }                                                                          \
    while (0)

  TEST_CASE ("create, switch, write, read, insert, remove, delete")
  {
    struct type type;
    compile_type (&type, "u32", &ctx->alloc, &ctx->e);

    check_state (0, 0);
    ns_ref_create (ref, "var1", &type, &ctx->e);
    check_state (1, 0);
    ns_ref_create (ref, "var2", &type, &ctx->e);
    check_state (2, 0);
    ns_ref_create (ref, "var3", &type, &ctx->e);
    check_state (3, 0);

    // Current variable is var1

    ns_ref_begin_txn (ref, &ctx->e);
    {
      ns_ref_insert (ref, (u32[]){10, 20, 30, 40}, 0, 4, &ctx->e);
      validate (((u32[]){10, 20, 30, 40}));
      check_state (3, 4 * sizeof (u32));

      ns_ref_insert (ref, (u32[]){50, 60}, 4, 2, &ctx->e);
      validate (((u32[]){10, 20, 30, 40, 50, 60}));
      check_state (3, 6 * sizeof (u32));

      ns_ref_insert (ref, (u32[]){70}, 6, 1, &ctx->e);
      validate (((u32[]){10, 20, 30, 40, 50, 60, 70}));
      check_state (3, 7 * sizeof (u32));

      // 100 20 200 40 300 60 400
      ns_ref_write (
          ref,
          (u32[]){100, 200, 300, 400},
          (struct stride){.start = 0, .stride = 2, .nelems = 4}
      );
      validate (((u32[]){100, 20, 200, 40, 300, 60, 400}));
      check_state (3, 7 * sizeof (u32));

      // 20 40 60
      ns_ref_remove (ref, dest, (struct stride){.start = 0, .stride = 2, .nelems = 4});
      test_assert_memequal (((u32[]){100, 200, 300, 400}), dest, 4 * sizeof (u32));
      validate (((u32[]){20, 40, 60}));
      check_state (3, 3 * sizeof (u32));
    }
    ns_ref_commit_txn (ref, &ctx->e);
  }

  TEST_CASE ("rollback restores prior state, including current variable")
  {
    ns_ref_switch (ref, "var2");
    ns_ref_insert (ref, (u32[]){10, 20, 30, 40}, 0, 4, &ctx->e);
    check_state (3, 7 * sizeof (u32));

    ns_ref_switch (ref, "var3");
    ns_ref_insert (ref, (u32[]){100, 200, 300, 400}, 0, 4, &ctx->e);
    check_state (3, 11 * sizeof (u32));

    ns_ref_switch (ref, "var1");

    // deletes var1 and switches to var2
    ns_ref_delete_and_switch (ref, "var2");
    validate (((u32[]){10, 20, 30, 40}));
    check_state (2, 8 * sizeof (u32));

    ns_ref_begin_txn (ref, &ctx->e);
    {
      ns_ref_switch (ref, "var3");
      ns_ref_insert (ref, (u32[]){111, 222, 333, 444}, 0, 4, &ctx->e);
      validate (((u32[]){111, 222, 333, 444, 100, 200, 300, 400}));
      check_state (2, 12 * sizeof (u32));
    }
    ns_ref_rollback_txn (ref);

    // Still var2
    validate (((u32[]){10, 20, 30, 40}));
    check_state (2, 8 * sizeof (u32));

    // var3 didn't get changes
    ns_ref_switch (ref, "var3");
    validate (((u32[]){100, 200, 300, 400}));
    check_state (2, 8 * sizeof (u32));
  }

  TEST_CASE ("commit persists the switch and the write")
  {
    ns_ref_begin_txn (ref, &ctx->e);
    {
      ns_ref_switch (ref, "var2");
      ns_ref_write (
          ref,
          (u32[]){111, 222, 333, 444},
          (struct stride){.start = 0, .stride = 1, .nelems = 4}
      );
      check_state (2, 8 * sizeof (u32));
      validate (((u32[]){111, 222, 333, 444}));
    }
    ns_ref_commit_txn (ref, &ctx->e);

    // Still on var2
    validate (((u32[]){111, 222, 333, 444}));
    check_state (2, 8 * sizeof (u32));

    // var3 didn't get changes
    ns_ref_switch (ref, "var3");
    validate (((u32[]){100, 200, 300, 400}));
    check_state (2, 8 * sizeof (u32));
  }

  arena_alloc_free_all (&ctx->alloc);
  ns_ref_free (ref);
}

#endif
