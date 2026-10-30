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

#include "nscore/types/ns_sarray_t.h"

#include "core/ns_arena_alloc.h"
#include "core/ns_bounds.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_numerics.h"
#include "core/ns_serial.h"
#include "core/ns_utils.h"
#include "nscore/types/ns_types.h"

#ifndef NDEBUG
#  include "core/ns_testing.h"
#endif

#include <stdio.h>
#include <string.h>

/******************************************************************************
 * SECTION: Strict Array Builder
 ******************************************************************************/

DEFINE_DBG_ASSERT (struct sarray_builder, sarray_builder, s, { ASSERT (s); })

struct sarray_builder
sab_create (struct builder *b)
{
  return (struct sarray_builder){
      .head = NULL,
      .type = NULL,
      .b    = b,
  };
}
DEFINE_DBG_ASSERT (struct sarray_t, unchecked_sarray_t, s, {
  ASSERT (s);
  ASSERT (s->dims);
  ASSERT (s->t);
})

static err_t
sarray_t_type_err (const char *msg, error *e)
{
  return error_causef (e, ERR_INTERP, "Strict Array: %s", msg);
}

static err_t
sarray_t_type_deser (const char *msg, error *e)
{
  return error_causef (e, ERR_CORRUPT, "Strict Array: %s", msg);
}

static err_t
sarray_t_validate_shallow (const struct sarray_t *t, error *e)
{
  DBG_ASSERT (unchecked_sarray_t, t);
  if (t->rank == 0) {
    return sarray_t_type_err ("Rank must be > 0", e);
  }
  for (u32 i = 0; i < t->rank; ++i) {
    if (t->dims[i] == 0) {
      return sarray_t_type_err ("dimensions cannot be 0", e);
    }
  }
  return SUCCESS;
}

#ifndef NDEBUG
TEST (sarray_t_validate_shallow)
{
  BUILDER_INIT (b, &ctx->alloc);

  TEST_CASE ("rank == 0")
  {
    struct sarray_t s = {
        .dims = (u32[]){1},
        .rank = 0,
        .t    = &TU8,
    };
    test_err_t_check (sarray_t_validate_shallow (&s, &ctx->e), ERR_INTERP, &ctx->e);
  }

  TEST_CASE ("Null dimension")
  {
    struct sarray_t s = {
        .dims = (u32[]){10, 0, 11},
        .rank = 3,
        .t    = &TU8,
    };
    test_err_t_check (sarray_t_validate_shallow (&s, &ctx->e), ERR_INTERP, &ctx->e);
  }

  BUILDER_CLOSE (b);
  arena_alloc_free_all (&ctx->alloc);
}
#endif

err_t
sarray_t_validate (const struct sarray_t *t, error *e)
{
  DBG_ASSERT (unchecked_sarray_t, t);

  WRAP (sarray_t_validate_shallow (t, e));
  WRAP (type_validate (t->t, e));

  return SUCCESS;
}

DEFINE_DBG_ASSERT (struct sarray_t, valid_sarray_t, s, {
  error e = error_create ();
  ASSERT (sarray_t_validate (s, &e) == SUCCESS);
})

i32
sarray_t_snprintf (char *str, u32 size, const struct sarray_t *p)
{
  DBG_ASSERT (valid_sarray_t, p);

  char *out   = str;
  u32   avail = size;
  int   len   = 0;
  int   n;

  for (u16 i = 0; i < p->rank; ++i) {
    n = snprintf (out, avail, "[%u]", p->dims[i]);
    if (n < 0) {
      return n;
    }
    len += n;
    if (out) {
      out += n;
      if ((u32)n < avail) {
        avail -= n;
      } else {
        avail = 0;
      }
    }
  }

  n = type_snprintf (out, avail, p->t);
  if (n < 0) {
    return n;
  }
  len += n;

  return len;
}

#ifndef NDEBUG
TEST (sarray_t_snprintf)
{
  struct type s = (struct type){
      .type = T_SARRAY,
      .sa   = {
          .dims = (u32[]){10, 11, 12},
          .rank = 3,
          .t    = &(struct type){
              .type = T_PRIM,
              .p    = U32,
          },
      },
  };

  const char *expected = "[10][11][12]u32";

  char       *ret      = type_tostr (&ctx->alloc, &s, &ctx->e);
  i_log_type (&s, &ctx->e);
  test_assert_int_equal (strncmp (expected, ret, strlen (expected)), 0);
  arena_alloc_free_all (&ctx->alloc);
}
#endif

u32
sarray_t_byte_size (const struct sarray_t *t)
{
  DBG_ASSERT (valid_sarray_t, t);
  u32 ret = 1;

  // multiply up all ranks and multiply by size of type
  for (u32 i = 0; i < t->rank; ++i) {
    ret *= t->dims[i];
  }

  return ret * type_byte_size (t->t);
}

#ifndef NDEBUG
TEST (sarray_t_byte_size)
{
  struct sarray_t s = {
      .dims = (u32[]){10, 11, 12},
      .rank = 3,
      .t    = &(struct type){
          .type = T_PRIM,
          .p    = U32,
      },
  };

  struct type t = {
      .type = T_SARRAY,
      .sa   = s,
  };
  u64 act = type_byte_size (&t);
  test_assert_int_equal (act, 10 * 11 * 12 * 4);
}
#endif

err_t
sarray_t_get_serial_size (u16 *dest, const struct sarray_t *t, error *e)
{
  DBG_ASSERT (valid_sarray_t, t);

  // RANK DIM0 DIM1 DIM2 ... TYPE
  u16 ret = sizeof (u16);
  WRAP (safe_add_mul_u16_err (&ret, sizeof (u32), t->rank, e));

  u16 sub_size;
  WRAP (type_get_serial_size (&sub_size, t->t, e));
  WRAP (safe_add_u16_err (&ret, sub_size, e));

  *dest = ret;

  return ret;
}

void
sarray_t_serialize (struct serializer *persistent, const struct sarray_t *src)
{
  DBG_ASSERT (valid_sarray_t, src);
  bool ret;
  (void)ret; // Unused in release

  // RANK DIM0 DIM1 DIM2 ... TYPE
  ret = srlizr_write (persistent, (const u8 *)&src->rank, sizeof (u16));
  ASSERT (ret);

  for (u32 i = 0; i < src->rank; ++i) {
    // DIMi
    ret = srlizr_write (persistent, (const u8 *)&src->dims[i], sizeof (u32));
    ASSERT (ret);
  }

  // (TYPE)
  type_serialize (persistent, src->t);
}

#ifndef NDEBUG
TEST (sarray_t_serialize)
{
  struct sarray_t s = {
      .dims = (u32[]){10, 11, 12},
      .rank = 3,
      .t    = &(struct type){
          .type = T_PRIM,
          .p    = U32,
      },
  };

  u8  act[200];
  u8  exp[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)T_PRIM, (u8)U32};
  u16 len   = 3;
  u32 d0    = 10;
  u32 d1    = 11;
  u32 d2    = 12;
  memcpy (exp, &len, 2);
  memcpy (exp + 2, &d0, 4);
  memcpy (exp + 6, &d1, 4);
  memcpy (exp + 10, &d2, 4);

  struct serializer sr = srlizr_create (act, 200);
  sarray_t_serialize (&sr, &s);

  test_assert_int_equal (sr.dlen, sizeof (exp));
  test_assert_int_equal (memcmp (act, exp, sizeof (exp)), 0);
}
#endif

#ifndef NDEBUG
TEST (sarray_t_get_serial_size)
{
  struct sarray_t s = {
      .dims = (u32[]){10, 11, 12},
      .rank = 3,
      .t    = &(struct type){
          .type = T_PRIM,
          .p    = U32,
      },
  };

  u16 dest;
  sarray_t_get_serial_size (&dest, &s, &ctx->e);

  test_assert_int_equal (dest, 3 * 4 + 2 + 2);
}
#endif

err_t
sarray_t_deserialize (
    struct sarray_t     *persistent,
    struct deserializer *src,
    struct arena_alloc  *a,
    error               *e
)
{
  ASSERT (persistent);

  struct sarray_t sa = {0};

  // RANK
  if (!dsrlizr_read ((u8 *)&sa.rank, sizeof (u16), src)) {
    goto early_terimination;
  }

  // Allocate dimensions buffer
  u32 *dims = arena_malloc (a, sa.rank, sizeof *dims, e);
  if (dims == NULL) {
    return error_trace (e);
  }
  sa.dims        = dims;

  // Allocate type
  struct type *t = arena_malloc (a, 1, sizeof *t, e);
  if (t == NULL) {
    return error_trace (e);
  }
  sa.t = t;

  for (u32 i = 0; i < sa.rank; ++i) {
    u32 dim;

    // DIMi
    if (!dsrlizr_read ((u8 *)&dim, sizeof (u32), src)) {
      goto early_terimination;
    }

    sa.dims[i] = dim;
  }

  // (TYPE)
  sa.t = type_deserialize (src, a, e);
  if (sa.t == NULL) {
    return error_trace (e);
  }
  WRAP (sarray_t_validate_shallow (&sa, e));

  *persistent = sa;
  return SUCCESS;

early_terimination:
  return sarray_t_type_deser ("Early end of serialized string", e);
}

#ifndef NDEBUG
TEST (sarray_t_deserialize_green_path)
{
  u8  data[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)T_PRIM, (u8)U32};
  u16 len    = 3;
  u32 d0     = 10;
  u32 d1     = 11;
  u32 d2     = 12;
  memcpy (data, &len, 2);
  memcpy (data + 2, &d0, 4);
  memcpy (data + 6, &d1, 4);
  memcpy (data + 10, &d2, 4);

  struct deserializer d    = dsrlizr_create (data, sizeof (data));

  struct sarray_t     sret = {0};
  err_t               ret  = sarray_t_deserialize (&sret, &d, &ctx->alloc, &ctx->e);

  test_assert_int_equal (ret, SUCCESS);

  test_assert_int_equal (sret.rank, 3);

  test_assert_int_equal (sret.dims[0], 10);
  test_assert_int_equal (sret.dims[1], 11);
  test_assert_int_equal (sret.dims[2], 12);

  arena_alloc_free_all (&ctx->alloc);
}
#endif

#ifndef NDEBUG
TEST (sarray_t_deserialize_red_path)
{
  u8  data[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)T_PRIM, (u8)U32};
  u16 len    = 3;
  u32 d0     = 10;
  u32 d1     = 0;
  u32 d2     = 12;
  memcpy (data, &len, 2);
  memcpy (data + 2, &d0, 4);
  memcpy (data + 6, &d1, 4);
  memcpy (data + 10, &d2, 4);

  struct sarray_t     eret;
  struct deserializer d   = dsrlizr_create (data, sizeof (data));

  err_t               ret = sarray_t_deserialize (&eret, &d, &ctx->alloc, &ctx->e);

  test_assert_int_equal (ret, ERR_INTERP); // 0 value

  arena_alloc_free_all (&ctx->alloc);
}
#endif

#ifndef NDEBUG
TEST (sarray_t_deserialize_early_termination)
{
  TEST_CASE ("Truncated rank")
  {
    u8                  data[] = {0}; // One byte of a two byte rank
    struct deserializer d      = dsrlizr_create (data, sizeof (data));

    struct sarray_t     sret   = {0};
    test_err_t_check (sarray_t_deserialize (&sret, &d, &ctx->alloc, &ctx->e), ERR_CORRUPT, &ctx->e);

    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Truncated dimension")
  {
    // rank = 2, then three of the eight dimension bytes
    u8  data[] = {0, 0, 0, 0, 0};
    u16 rank   = 2;
    memcpy (data, &rank, sizeof rank);

    struct deserializer d    = dsrlizr_create (data, sizeof (data));

    struct sarray_t     sret = {0};
    test_err_t_check (sarray_t_deserialize (&sret, &d, &ctx->alloc, &ctx->e), ERR_CORRUPT, &ctx->e);

    arena_alloc_free_all (&ctx->alloc);
  }

  TEST_CASE ("Dimensions complete, type missing")
  {
    // rank = 1, dim0 = 10, and nothing where the element type should be
    u8  data[] = {0, 0, 0, 0, 0, 0};
    u16 rank   = 1;
    u32 d0     = 10;
    memcpy (data, &rank, sizeof rank);
    memcpy (data + 2, &d0, sizeof d0);

    struct deserializer d    = dsrlizr_create (data, sizeof (data));

    struct sarray_t     sret = {0};
    test_assert (sarray_t_deserialize (&sret, &d, &ctx->alloc, &ctx->e) != SUCCESS);
    error_reset (&ctx->e);

    arena_alloc_free_all (&ctx->alloc);
  }
}
#endif

err_t
sarray_t_random (
    struct sarray_t    *sa,
    struct arena_alloc *temp,
    u32                 depth,
    t_size              max_tsize,
    error              *e
)
{
  ASSERT (sa);

  sa->rank = (u16)randu32r (1, 4);

  sa->dims = (u32 *)arena_malloc (temp, sa->rank, sizeof (u32), e);
  if (!sa->dims) {
    return error_trace (e);
  }

  for (u16 i = 0; i < sa->rank; ++i) {
    sa->dims[i] = randu32r (1, 11);
  }

  sa->t = type_random (temp, depth - 1, max_tsize, e);
  if (sa->t == NULL) {
    return error_trace (e);
  }

  return SUCCESS;
}

bool
sarray_t_equal (const struct sarray_t *left, const struct sarray_t *right)
{
  if (left->rank != right->rank) {
    return false;
  }

  for (u32 i = 0; i < left->rank; ++i) {
    if (left->dims[i] != right->dims[i]) {
      return false;
    }
  }

  return type_equal (left->t, right->t);
}

err_t
sab_accept_dim (struct sarray_builder *eb, i32 dim, error *e)
{
  DBG_ASSERT (sarray_builder, eb);

  if (dim <= 0) {
    return error_causef (e, ERR_SYNTAX, "sarray dimension must be > 0");
  }

  u16                idx  = (u16)list_length (eb->head);
  struct llnode     *slot = llnode_get_n (eb->head, idx);
  struct dim_llnode *node;

  if (slot) {
    node = container_of (slot, struct dim_llnode, link);
  } else {
    node = builder_malloc_temp (eb->b, 1, sizeof *node, e);
    if (!node) {
      return error_trace (e);
    }
    llnode_init (&node->link);
    if (!eb->head) {
      eb->head = &node->link;
    } else {
      list_append (&eb->head, &node->link);
    }
  }

  node->dim = dim;
  return SUCCESS;
}

err_t
sab_accept_type (struct sarray_builder *eb, struct type *t, error *e)
{
  DBG_ASSERT (sarray_builder, eb);

  if (eb->type) {
    return error_causef (e, ERR_INTERP, "type already set");
  }

  eb->type = type_movemem (t, eb->b->persistent, e);

  return error_trace (e);
}

err_t
sab_build (struct sarray_t *persistent, struct sarray_builder *eb, error *e)
{
  DBG_ASSERT (sarray_builder, eb);
  ASSERT (persistent);

  if (!eb->type) {
    error_causef (e, ERR_INTERP, "type not set");
    goto theend;
  }

  u16 rank = (u16)list_length (eb->head);
  if (rank == 0) {
    error_causef (e, ERR_INTERP, "no dims to build");
    goto theend;
  }

  // If the element type is itself an sarray, flatten it: our dims come
  // first, then the sub-array's dims, and the element type collapses to
  // the sub-array's (already-flat) element type.
  struct sarray_t *sub      = NULL;
  u16              sub_rank = 0;
  if (eb->type->type == T_SARRAY) {
    sub      = &eb->type->sa;
    sub_rank = sub->rank;
  }

  u16  total_rank = rank + sub_rank;
  u32 *dims       = builder_malloc_persist (eb->b, total_rank, sizeof *dims, e);
  if (!dims) {
    goto theend;
  }

  // Copy type to persistent memory (eb->type is on temp)
  struct type *t = builder_malloc_persist (eb->b, 1, sizeof *t, e);
  if (!t) {
    goto theend;
  }
  *t    = sub ? *sub->t : *eb->type;

  u16 i = 0;
  for (struct llnode *it = eb->head; it; it = it->next) {
    struct dim_llnode *dn = container_of (it, struct dim_llnode, link);
    dims[i++]             = dn->dim;
  }
  for (u16 j = 0; j < sub_rank; j++) {
    dims[i++] = sub->dims[j];
  }

  persistent->rank = total_rank;
  persistent->dims = dims;
  persistent->t    = t;

theend:
  return error_trace (e);
}

#ifndef NDEBUG
TEST (sarray_builder)
{
  BUILDER_INIT (b, &ctx->alloc);

  // provide two fixed-size allocators for nodes + dims array

  // 0. freshly-created builder must be clean
  struct sarray_builder sb = sab_create (&b);
  test_fail_if (sb.head != NULL);
  test_fail_if (sb.type != NULL);

  // 1. build without type -> ERR_INTERP
  struct sarray_t sar = {0};
  test_assert_int_equal (sab_build (&sar, &sb, &ctx->e), ERR_INTERP);
  ctx->e.cause_code = SUCCESS;

  // 2. set type but no dims -> still ERR_INTERP
  struct type t_u32 = (struct type){.type = T_PRIM, .p = U32};
  test_assert_int_equal (sab_accept_type (&sb, &t_u32, &ctx->e), SUCCESS);
  test_assert_int_equal (sab_build (&sar, &sb, &ctx->e), ERR_INTERP);
  error_reset (&ctx->e);

  // 3. duplicate type must fail
  test_assert_int_equal (sab_accept_type (&sb, &t_u32, &ctx->e), ERR_INTERP);
  error_reset (&ctx->e);

  // 4. accept first dim 10
  test_assert_int_equal (sab_accept_dim (&sb, 10, &ctx->e), SUCCESS);

  // 5. successful build now that we have type and one dim
  test_assert_int_equal (sab_build (&sar, &sb, &ctx->e), SUCCESS);
  test_assert_int_equal (sar.rank, 1);
  test_assert_int_equal (*sar.dims, 10);
  test_assert_int_equal (sar.t->p, t_u32.p);

  // 6. accept additional dims and rebuild (rank 3)
  test_assert_int_equal (sab_accept_dim (&sb, 4, &ctx->e), SUCCESS);
  test_assert_int_equal (sab_accept_dim (&sb, 2, &ctx->e), SUCCESS);
  test_assert_int_equal (sab_build (&sar, &sb, &ctx->e), SUCCESS);
  test_assert_int_equal (sar.rank, 3);
  test_assert_int_equal (sar.dims[0], 10);
  test_assert_int_equal (sar.dims[1], 4);
  test_assert_int_equal (sar.dims[2], 2);

  // 7. element type that is itself an sarray must be flattened into the
  //    top-level sarray (dims concatenated, element type collapsed)
  struct sarray_builder nb          = sab_create (&b);

  struct type           inner_elem  = (struct type){.type = T_PRIM, .p = U32};
  u32                   sub_dims[2] = {3, 5};
  struct type           sub_ty      = (struct type){
      .type = T_SARRAY,
      .sa   = {.rank = 2, .dims = sub_dims, .t = &inner_elem},
  };

  test_assert_int_equal (sab_accept_type (&nb, &sub_ty, &ctx->e), SUCCESS);
  test_assert_int_equal (sab_accept_dim (&nb, 10, &ctx->e), SUCCESS);

  struct sarray_t flat = {0};
  test_assert_int_equal (sab_build (&flat, &nb, &ctx->e), SUCCESS);
  test_assert_int_equal (flat.rank, 3);
  test_assert_int_equal (flat.dims[0], 10);
  test_assert_int_equal (flat.dims[1], 3);
  test_assert_int_equal (flat.dims[2], 5);
  test_assert_int_equal (flat.t->type, T_PRIM);
  test_assert_int_equal (flat.t->p, U32);

  BUILDER_CLOSE (b);
  arena_alloc_free_all (&ctx->alloc);
}
#endif
