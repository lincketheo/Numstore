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

#include "nscore/testing/simulation/ns_mem_vhmap.h"

#include "core/ns_arena_alloc.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_ext_array.h"
#include "core/ns_htable.h"
#include "core/ns_numerics.h"
#include "core/ns_slab_alloc.h"
#include "core/ns_string.h"
#include "core/ns_testing.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"
#include "nscore/types/ns_types.h"
#include "nscore/variables/ns_variables.h"

#include <string.h>

struct var_frame
{
  struct var_with_data var;
  struct hnode         node;
  struct arena_alloc   alloc;
};

// Lifecycle
struct mem_vhmap *
mem_vhmap_create (struct i_mem mem, error *e)
{
  struct mem_vhmap *ret = mem.table->malloc (mem.self, 1, sizeof *ret, e);
  if (ret == NULL) {
    return NULL;
  }

  ret->vhasht = htable_create (256, mem, e);
  if (ret->vhasht == NULL) {
    mem.table->free (mem.self, ret);
    return NULL;
  }

  ret->mem = mem;
  slab_alloc_init (&ret->alloc, mem, sizeof (struct var_frame), 256);

  return ret;
}

static void
var_frame_free (struct var_frame *frame)
{
  arena_alloc_free_all (&frame->alloc);
  ext_array_free (&frame->var.data);
}

static void
var_frame_free_hnode (struct hnode *node, void *ctx)
{
  (void)ctx; // Unused
  struct var_frame *frame = container_of (node, struct var_frame, node);
  var_frame_free (frame);
}

void
mem_vhmap_free (struct mem_vhmap *db)
{
  ASSERT (db);
  htable_foreach (db->vhasht, var_frame_free_hnode, NULL);
  slab_alloc_destroy (&db->alloc);
  htable_free (db->vhasht);
  db->mem.table->free (db->mem.self, db);
}

struct copy_ctx
{
  struct mem_vhmap *dest;
  error            *e;
};

static void
move_data (struct hnode *node, void *ctx)
{
  struct copy_ctx *_ctx = ctx;
  if (_ctx->e->cause_code < 0) {
    return;
  }

  // Fetch the next variable
  struct var_frame     *frame = container_of (node, struct var_frame, node);

  // Insert it into the destination
  struct var_with_data *var   = mem_vhmap_add (_ctx->dest, &frame->var.var, _ctx->e);
  if (var == NULL) {
    return;
  }

  // Copy the data over
  ext_array_free (&var->data);
  ext_array_clone (&var->data, &frame->var.data, _ctx->e);
}

struct mem_vhmap *
mem_vhmap_clone (struct i_mem mem, const struct mem_vhmap *src, error *e)
{
  // Create a new var hash map
  struct mem_vhmap *ret = mem_vhmap_create (mem, e);
  if (ret == NULL) {
    return NULL;
  }

  // Run copy for each variable in src
  struct copy_ctx ctx = {
      .dest = ret,
      .e    = e,
  };
  htable_foreach (src->vhasht, move_data, &ctx);

  // Error
  if (ctx.e->cause_code) {
    mem_vhmap_free (ret);
    return NULL;
  }

  return ret;
}

static bool
vframe_eq (const struct hnode *left, const struct hnode *right)
{
  struct var_frame *_left  = container_of (left, struct var_frame, node);
  struct var_frame *_right = container_of (right, struct var_frame, node);
  return string_equal (_left->var.var.vname, _right->var.var.vname);
}

static err_t
var_frame_init (struct mem_vhmap *db, struct var_frame *frame, struct variable *var, error *e)
{
  // Create an allocator for this variable
  arena_alloc_create_default (&frame->alloc);

  // Copy variable data over to
  if (variable_copy (&frame->var.var, var, &frame->alloc, e)) {
    goto failed;
  }

  // Create the block array
  frame->var.data = ext_array_create (db->mem);

  return SUCCESS;

failed:
  arena_alloc_free_all (&frame->alloc);
  return error_trace (e);
}

struct var_with_data *
mem_vhmap_add (struct mem_vhmap *db, struct variable *var, error *e)
{
  // Look up to see if there are
  // any conflicts
  struct var_frame key;
  memset (&key, 0, sizeof (key));
  key.var.var.vname = var->vname;
  hnode_init (&key.node, fnv1a_hash (var->vname));
  struct hnode **found = htable_lookup (db->vhasht, &key.node, vframe_eq);

  if (found) {
    error_causef (e, ERR_DUPLICATE_VARIABLE, "Variable already exists");
    return NULL;
  }

  // Create a new variable frame
  struct var_frame *frame = slab_alloc_alloc (&db->alloc, e);
  if (frame == NULL) {
    return NULL;
  }

  // Initialize this frame
  if (var_frame_init (db, frame, var, e)) {
    slab_alloc_free (&db->alloc, frame);
    return NULL;
  }

  // Add this frame to the table
  hnode_init (&frame->node, fnv1a_hash (var->vname));
  htable_insert (db->vhasht, &frame->node);

  return &frame->var;
}

struct var_with_data *
mem_vhmap_get (struct mem_vhmap *db, struct string name)
{
  // Lookup this variable
  struct var_frame key;
  memset (&key, 0, sizeof (key));
  key.var.var.vname = name;
  hnode_init (&key.node, fnv1a_hash (name));
  struct hnode **found = htable_lookup (db->vhasht, &key.node, vframe_eq);
  if (found) {
    struct var_frame *var = container_of (*found, struct var_frame, node);
    return &var->var;
  }
  return NULL;
}

void
mem_vhmap_remove (struct mem_vhmap *db, struct string name)
{
  // Lookup this variable
  struct var_frame key;
  memset (&key, 0, sizeof (key));
  key.var.var.vname = name;
  hnode_init (&key.node, fnv1a_hash (name));

  struct hnode **found = htable_lookup (db->vhasht, &key.node, vframe_eq);

  ASSERT (found);

  // Free the variable frame
  struct var_frame *frame = container_of (*found, struct var_frame, node);
  var_frame_free (frame);

  htable_delete (db->vhasht, found);

  slab_alloc_free (&db->alloc, frame);
}

struct rand_ctx
{
  u32                   index;
  u32                   target;
  struct var_with_data *dest;
};

static void
random_iter (struct hnode *node, void *_ctx)
{
  struct rand_ctx *ctx = _ctx;

  // Already done
  if (ctx->dest) {
    return;
  }

  if (ctx->index == ctx->target) {
    ctx->dest = &container_of (node, struct var_frame, node)->var;
  }

  ctx->index++;
}

struct var_with_data *
mem_vhmap_random (struct mem_vhmap *db)
{
  struct rand_ctx ctx = {
      .index  = 0,
      .target = randu32r (1, MAX (htable_size (db->vhasht), 1)) - 1,
      .dest   = NULL,
  };
  htable_foreach (db->vhasht, random_iter, &ctx);
  return ctx.dest;
}

#ifndef NDEBUG
TEST (mem_vhmap)
{
  struct mem_vhmap *v       = mem_vhmap_create (ctx->mem, &ctx->e);

  struct type       deftype = {
      .type = T_PRIM,
      .p    = U32,
  };
  char buf[32];

  TEST_CASE ("every added var is immediately retrievable with correct data")
  {
    for (int i = 0; i < 10000; ++i) {
      snprintf (buf, sizeof buf, "var_%d", i);
      struct variable var = {
          .vname  = strfcstr (buf),
          .dtype  = &deftype,
          .nbytes = (b_size)i,
      };
      struct var_with_data *vwd = mem_vhmap_add (v, &var, &ctx->e);
      ASSERT (vwd != NULL);

      struct var_with_data *got = mem_vhmap_get (v, strfcstr (buf));
      ASSERT (got != NULL);
      ASSERT (got->var.nbytes == (b_size)i);
    }
  }

  TEST_CASE ("duplicate add fails")
  {
    {
      struct variable dup = {
          .vname = strfcstr ("var_0"),
          .dtype = &deftype,
      };
      ASSERT (mem_vhmap_add (v, &dup, &ctx->e) == NULL);
      error_reset (&ctx->e);
    }
  }

  TEST_CASE ("get on unknown name returns NULL")
  {
    ASSERT (mem_vhmap_get (v, strfcstr ("__no_such_var__")) == NULL);
  }

  TEST_CASE ("remove makes the var unretrievable; others unaffected")
  {
    for (int i = 0; i < 10000; ++i) {
      snprintf (buf, sizeof buf, "var_%d", i);
      struct string name = strfcstr (buf);
      mem_vhmap_remove (v, name);
      ASSERT (mem_vhmap_get (v, name) == NULL);

      // spot-check that the next var (if any) is still there
      if (i + 1 < 10000) {
        snprintf (buf, sizeof buf, "var_%d", i + 1);
        ASSERT (mem_vhmap_get (v, strfcstr (buf)) != NULL);
      }
    }
  }

  TEST_CASE ("clone captures state; mutations in clone don't affect original")
  {
    for (int i = 0; i < 128; ++i) {
      snprintf (buf, sizeof buf, "cv_%d", i);
      struct variable var = {
          .vname  = strfcstr (buf),
          .nbytes = (b_size)(i * 3),
          .dtype  = &deftype,
      };
      struct var_with_data *vwd = mem_vhmap_add (v, &var, &ctx->e);
      test_assert (vwd != NULL);
    }

    struct mem_vhmap *c = mem_vhmap_clone (ctx->mem, v, &ctx->e);
    ASSERT (c != NULL);

    for (int i = 0; i < 128; ++i) {
      snprintf (buf, sizeof buf, "cv_%d", i);
      struct var_with_data *got = mem_vhmap_get (c, strfcstr (buf));
      ASSERT (got != NULL);
      ASSERT (got->var.nbytes == (b_size)(i * 3));
    }

    mem_vhmap_remove (c, strfcstr ("cv_0"));
    ASSERT (mem_vhmap_get (c, strfcstr ("cv_0")) == NULL); // removed in clone
    ASSERT (mem_vhmap_get (v, strfcstr ("cv_0")) != NULL); // original intact

    mem_vhmap_free (c);
  }

  mem_vhmap_free (v);
}
#endif
