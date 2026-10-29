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

#include "nscore/types/ns_types.h"

#include "core/ns_arena_alloc.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_numerics.h"
#include "core/ns_serial.h"
#include "core/ns_stream.h"
#include "core/ns_string.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"
#include "nscore/types/ns_sarray_t.h"
#include "nscore/types/ns_struct_t.h"
#include "nscore/types/ns_type_ref.h"
#include "nscore/types/ns_union_t.h"

#include <stddef.h>

#ifndef NDEBUG
#  include "core/ns_testing.h"
#endif

#include <stdio.h>
#include <string.h>

static inline err_t
prim_t_validate (const enum prim_t *t, error *e)
{
  ASSERT (t);
  if (!(*t <= CU128 && *t >= U8)) {
    return error_causef (e, ERR_INTERP, "invalid prim type %d (valid range %d..%d)", *t, U8, CU128);
  }

  return SUCCESS;
}

DEFINE_DBG_ASSERT (enum prim_t, prim_t, s, {
  ASSERT (s);
  error e = error_create ();
  ASSERT (prim_t_validate (s, &e) == SUCCESS);
})

#ifndef NDEBUG
TEST (prim_t_validate)
{
  // 1.1 happy path – legal value
  enum prim_t ok = U32;
  test_assert_int_equal (prim_t_validate (&ok, &ctx->e), SUCCESS);

  // 1.2 too‑small → ERR_INTERP
  enum prim_t bad_lo = (enum prim_t) (U8 - 1);
  test_assert_int_equal (prim_t_validate (&bad_lo, &ctx->e), ERR_INTERP);
  ctx->e.cause_code  = SUCCESS;

  // 1.3 too‑large → ERR_INTERP
  enum prim_t bad_hi = (enum prim_t) (CU128 + 1);
  test_assert_int_equal (prim_t_validate (&bad_hi, &ctx->e), ERR_INTERP);
  error_reset (&ctx->e);
}
#endif

err_t
type_validate (const struct type *t, error *e)
{
  DBG_ASSERT (unchecked_type, t);
  switch (t->type) {
    case T_PRIM: {
      WRAP (prim_t_validate (&t->p, e));
      break;
    }
    case T_STRUCT: {
      WRAP (struct_t_validate (&t->st, e));
      break;
    }
    case T_UNION: {
      WRAP (union_t_validate (&t->un, e));
      break;
    }
    case T_SARRAY: {
      WRAP (sarray_t_validate (&t->sa, e));
      break;
    }
    default: {
      // Reachable from on-disk data: type_deserialize casts a stored byte
      // straight to enum type_t, so a corrupt or truncated file lands here.
      // Report it rather than killing the process.
      return error_causef (e, ERR_CORRUPT, "Unknown type code: %d", (int)t->type);
    }
  }

  // Check that it can be written to a serial buffer
  u16 size;
  return type_get_serial_size (&size, t, e);
}

/*-----------------------------------------------------------------------------
 * SUBSECTION: type_snprintf
 * @brief Print a type into a buffer cleanly
 *----------------------------------------------------------------------------*/

static const char *
prim_to_str (enum prim_t p)
{
  DBG_ASSERT (prim_t, &p);
  switch (p) {
    case U8: return "u8";
    case U16: return "u16";
    case U32: return "u32";
    case U64: return "u64";

    case I8: return "i8";
    case I16: return "i16";
    case I32: return "i32";
    case I64: return "i64";

    case F16: return "f16";
    case F32: return "f32";
    case F64: return "f64";
    case F128: return "f128";

    case CF32: return "cf32";
    case CF64: return "cf64";
    case CF128: return "cf128";
    case CF256: return "cf256";

    case CI16: return "ci16";
    case CI32: return "ci32";
    case CI64: return "ci64";
    case CI128: return "ci128";

    case CU16: return "cu16";
    case CU32: return "cu32";
    case CU64: return "cu64";
    case CU128: return "cu128";
  }
  UNREACHABLE (); // LCOV_EXCL_LINE
  return "";      // LCOV_EXCL_LINE
}

static i32
prim_t_snprintf (char *str, u32 size, const enum prim_t p)
{
  DBG_ASSERT (prim_t, &p);

  char       *out   = str;
  u32         avail = size;
  int         len   = 0;
  int         n;
  const char *name = prim_to_str (p);

  n                = snprintf (out, avail, "%s", name);
  if (n < 0) {
    return n;
  }
  len += n;

  return len;
}

#ifndef NDEBUG
TEST (prim_t_snprintf)
{
#  define CASE_PRIM(prim_type, exp)                                      \
    TEST_CASE ("prim_t_snprintf(%s) == %s", #prim_type, exp)             \
    {                                                                    \
      struct type t = {                                                  \
          .type = T_PRIM,                                                \
          .p    = prim_type,                                             \
      };                                                                 \
                                                                         \
      const char *expect = exp;                                          \
      char       *ret    = type_tostr (&ctx->alloc, &t, &ctx->e);        \
      i_log_type (&t, &ctx->e);                                          \
      test_assert_int_equal (strncmp (expect, ret, strlen (expect)), 0); \
    }

  CASE_PRIM (U8, "u8");
  CASE_PRIM (U16, "u16");
  CASE_PRIM (U32, "u32");
  CASE_PRIM (U64, "u64");

  CASE_PRIM (I8, "i8");
  CASE_PRIM (I16, "i16");
  CASE_PRIM (I32, "i32");
  CASE_PRIM (I64, "i64");

  CASE_PRIM (F16, "f16");
  CASE_PRIM (F32, "f32");
  CASE_PRIM (F64, "f64");
  CASE_PRIM (F128, "f128");

  CASE_PRIM (CF32, "cf32");
  CASE_PRIM (CF64, "cf64");
  CASE_PRIM (CF128, "cf128");
  CASE_PRIM (CF256, "cf256");

  CASE_PRIM (CI16, "ci16");
  CASE_PRIM (CI32, "ci32");
  CASE_PRIM (CI64, "ci64");
  CASE_PRIM (CI128, "ci128");

  CASE_PRIM (CU16, "cu16");
  CASE_PRIM (CU32, "cu32");
  CASE_PRIM (CU64, "cu64");
  CASE_PRIM (CU128, "cu128");

  arena_alloc_free_all (&ctx->alloc);
}
#endif

i32
type_snprintf (char *str, u32 size, struct type *t)
{
  DBG_ASSERT (valid_type, t);

  switch (t->type) {
    case T_PRIM: {
      return prim_t_snprintf (str, size, t->p);
    }
    case T_STRUCT: {
      return struct_t_snprintf (str, size, &t->st);
    }
    case T_UNION: {
      return union_t_snprintf (str, size, &t->un);
    }
    case T_SARRAY: {
      return sarray_t_snprintf (str, size, &t->sa);
    }
    default: {
      UNREACHABLE (); // LCOV_EXCL_LINE
      return -1;      // LCOV_EXCL_LINE
    }
  }
}

char *
type_tostr (struct arena_alloc *alloc, struct type *t, error *e)
{
  int len = type_snprintf (NULL, 0, t);
  if (len < 0) {
    return NULL;
  }

  char *msg = arena_malloc (alloc, len + 1, 1, e);
  if (msg == NULL) {
    return NULL;
  }

  if (type_snprintf (msg, len + 1, t) < 0) {
    return NULL;
  }

  return msg;
}

/*-----------------------------------------------------------------------------
 * SUBSECTION: type_byte_size
 * @brief Calculate how many bytes a type takes up
 *----------------------------------------------------------------------------*/

static u32
prim_t_byte_size (const enum prim_t *t)
{
  DBG_ASSERT (prim_t, t);

  switch (*t) {
    case U8:
    case I8: return 1;

    case U16:
    case I16:
    case F16:
    case CI16:
    case CU16: return 2;

    case U32:
    case I32:
    case F32:
    case CF32:
    case CI32:
    case CU32: return 4;

    case U64:
    case I64:
    case F64:
    case CF64:
    case CI64:
    case CU64: return 8;

    case F128:
    case CF128:
    case CI128:
    case CU128: return 16;

    case CF256: return 32;
  }

  UNREACHABLE (); // LCOV_EXCL_LINE
  return 0;       // LCOV_EXCL_LINE
}

#ifndef NDEBUG
TEST (prim_t_byte_size)
{
  enum prim_t p1 = U8;
  enum prim_t p2 = CF128;
  enum prim_t p3 = CF256;

  test_assert_int_equal (prim_t_byte_size (&p1), 1);
  test_assert_int_equal (prim_t_byte_size (&p2), 16);
  test_assert_int_equal (prim_t_byte_size (&p3), 32);
}

#endif

u32
type_byte_size (const struct type *t)
{
  DBG_ASSERT (valid_type, t);

  switch (t->type) {
    case T_PRIM: {
      return prim_t_byte_size (&t->p);
    }

    case T_STRUCT: {
      return struct_t_byte_size (&t->st);
    }

    case T_UNION: {
      return union_t_byte_size (&t->un);
    }

    case T_SARRAY: {
      return sarray_t_byte_size (&t->sa);
    }

    default: {
      UNREACHABLE (); // LCOV_EXCL_LINE
      return 0;       // LCOV_EXCL_LINE
    }
  }
}

u32
type_get_string_size (const struct type *t)
{
  DBG_ASSERT (valid_type, t);

  switch (t->type) {
    case T_PRIM: {
      // Room for largest primitive name (e.g., "cf128") + 1 null terminator
      return sizeof ("cf128");
    }

    case T_STRUCT:
    case T_UNION: {
      // "struct { }" or "union { }"
      u32 base_len = (t->type == T_STRUCT) ? sizeof ("struct { }") : sizeof ("union { }");
      u32 sublen   = 0;

      // Accessing st or un identically since they share identical structural
      // layouts
      u16 len      = t->st.len;
      for (u16 i = 0; i < len; ++i) {
        // Size of the key text + " " + string length of subtype
        sublen += t->st.keys[i].len + 1 + type_get_string_size (t->st.types[i]);

        if (i < len - 1) {
          sublen += sizeof (", ") - 1;
        }
      }
      return base_len + sublen;
    }

    case T_SARRAY: {
      u32 sublen = 0;
      // Calculate brackets layout string overhead "[10][200]..."
      for (u32 i = 0; i < t->sa.rank; ++i) {
        // Accounts for digits up to 4 billion + brackets '[' and ']'
        sublen += sizeof ("[4294967295]") - 1;
      }
      // Add a single space separation " TYPE"
      return sublen + 1 + type_get_string_size (t->sa.t);
    }

    default: {
      UNREACHABLE (); // LCOV_EXCL_LINE
      return 0;       // LCOV_EXCL_LINE
    }
  }
}

static char *
type_generate_string_rec (char *dest, char *end, const struct type *t)
{
  switch (t->type) {
    case T_PRIM: {
      int n = snprintf (dest, (size_t)(end - dest), "%s", prim_to_str (t->p));
      return dest + (n > 0 ? n : 0);
    }
    case T_STRUCT:
    case T_UNION: {
      char *p = dest;
      int n = snprintf (p, (size_t)(end - p), "%s { ", (t->type == T_STRUCT) ? "struct" : "union");
      p += (n > 0 ? n : 0);

      u16 len = t->st.len;
      for (u16 i = 0; i < len; ++i) {
        n = snprintf (p, (size_t)(end - p), "%.*s ", t->st.keys[i].len, t->st.keys[i].data);
        p += (n > 0 ? n : 0);

        p = type_generate_string_rec (p, end, t->st.types[i]);

        if (i < len - 1) {
          n = snprintf (p, (size_t)(end - p), ", ");
          p += (n > 0 ? n : 0);
        }
      }
      n = snprintf (p, (size_t)(end - p), " }");
      p += (n > 0 ? n : 0);
      return p;
    }
    case T_SARRAY: {
      char *p = dest;
      for (u16 i = 0; i < t->sa.rank; ++i) {
        int n = snprintf (p, (size_t)(end - p), "[%u]", t->sa.dims[i]);
        p += (n > 0 ? n : 0);
      }
      int n = snprintf (p, (size_t)(end - p), " ");
      p += (n > 0 ? n : 0);
      return type_generate_string_rec (p, end, t->sa.t);
    }
    default: {
      UNREACHABLE (); // LCOV_EXCL_LINE
      return dest;    // LCOV_EXCL_LINE
    }
  }
}

// Public interface function entry point
void
type_generate_string (char *dest, const struct type *t)
{
  DBG_ASSERT (valid_type, t);
  if (dest) {
    type_generate_string_rec (dest, dest + type_get_string_size (t), t);
  }
}

#ifndef NDEBUG
TEST (type_generate_string)
{
  TEST_CASE ("primitive")
  {
    struct type t            = {.type = T_PRIM, .p = CF128};
    const char *expected     = "cf128";
    u32         expected_len = (u32)strlen (expected);

    char        buf[64];
    type_generate_string (buf, &t);

    test_assert_int_equal (memcmp (buf, expected, expected_len + 1) == 0, 1);
  }

  TEST_CASE ("sarray")
  {
    struct type element      = {.type = T_PRIM, .p = I32};
    u32         dims[3]      = {5, 20, 100};
    struct type t            = {.type = T_SARRAY, .sa = {.rank = 3, .dims = dims, .t = &element}};
    const char *expected     = "[5][20][100] i32";
    u32         expected_len = (u32)strlen (expected);

    char        buf[64];
    type_generate_string (buf, &t);

    test_assert_int_equal (memcmp (buf, expected, expected_len + 1) == 0, 1);
  }

  TEST_CASE ("struct")
  {
    struct string keys[2]      = {{.data = "x", .len = 1}, {.data = "y", .len = 1}};
    struct type   f1           = {.type = T_PRIM, .p = F32};
    struct type   f2           = {.type = T_PRIM, .p = F32};
    struct type  *types[2]     = {&f1, &f2};

    struct type   t            = {.type = T_STRUCT, .st = {.len = 2, .keys = keys, .types = types}};
    const char   *expected     = "struct { x f32, y f32 }";
    u32           expected_len = (u32)strlen (expected);

    char          buf[64];
    type_generate_string (buf, &t);

    test_assert_int_equal (memcmp (buf, expected, expected_len + 1) == 0, 1);
  }

  TEST_CASE ("union")
  {
    struct string keys[2]  = {{.data = "as_int", .len = 6}, {.data = "as_ptr", .len = 6}};
    struct type   f1       = {.type = T_PRIM, .p = I64};
    struct type   f2       = {.type = T_PRIM, .p = U64};
    struct type  *types[2] = {&f1, &f2};

    struct type   t        = {
        .type = T_UNION,
        .un   = {.len = 2, .keys = keys, .types = types} // Using .un overlay explicitly
    };
    const char *expected     = "union { as_int i64, as_ptr u64 }";
    u32         expected_len = (u32)strlen (expected);

    char        buf[128];
    type_generate_string (buf, &t);

    test_assert_int_equal (memcmp (buf, expected, expected_len + 1) == 0, 1);
  }

  TEST_CASE ("complex_nested")
  {
    // Sub-component A: union { raw u8, state i32 }
    struct string un_keys[2]  = {{.data = "raw", .len = 3}, {.data = "state", .len = 5}};
    struct type   prim_u8     = {.type = T_PRIM, .p = U8};
    struct type   prim_i32    = {.type = T_PRIM, .p = I32};
    struct type  *un_types[2] = {&prim_u8, &prim_i32};
    struct type   inner_union = {
        .type = T_UNION,
        .un   = {.len = 2, .keys = un_keys, .types = un_types}
    };

    // Sub-component B: [5] cf32
    struct type prim_cf32         = {.type = T_PRIM, .p = CF32};
    u32         inner_arr_dims[1] = {5};
    struct type inner_array       = {
        .type = T_SARRAY,
        .sa   = {.rank = 1, .dims = inner_arr_dims, .t = &prim_cf32}
    };

    // Parent Struct: struct { payload <union>, tags <array> }
    struct string st_keys[2]    = {{.data = "payload", .len = 7}, {.data = "tags", .len = 4}};
    struct type  *st_types[2]   = {&inner_union, &inner_array};
    struct type   parent_struct = {
        .type = T_STRUCT,
        .st   = {.len = 2, .keys = st_keys, .types = st_types}
    };

    // Root Array: [2] <struct>
    u32         root_dims[1] = {2};
    struct type root_type    = {
        .type = T_SARRAY,
        .sa   = {.rank = 1, .dims = root_dims, .t = &parent_struct}
    };

    const char *expected     = "[2] struct { payload union { raw u8, state i32 }, tags [5] cf32 }";
    u32         expected_len = (u32)strlen (expected);

    // Verify type_get_string_size returns enough space for safe serialization
    u32         calculated_size = type_get_string_size (&root_type);
    test_assert_int_equal (calculated_size >= expected_len + 1, 1);

    char buf[256];
    type_generate_string (buf, &root_type);

    test_assert_int_equal (memcmp (buf, expected, expected_len + 1) == 0, 1);
  }
}
#endif

/*-----------------------------------------------------------------------------
 * SUBSECTION: type_get_serial_size
 * @brief Get the amount of bytes needed to serialize a type
 *----------------------------------------------------------------------------*/

err_t
type_get_serial_size (u16 *dest, const struct type *t, error *e)
{
  // DBG_ASSERT (valid_type, t);

  // LABEL TYPE
  u16 ret = sizeof (u8);

  switch (t->type) {
    case T_PRIM: {
      WRAP (safe_add_u16 (&ret, sizeof (u8), e));
      *dest = ret;
      return SUCCESS;
    }
    case T_STRUCT: {
      u16 sub_size;
      WRAP (struct_t_get_serial_size (&sub_size, &t->st, e));
      WRAP (safe_add_u16 (&ret, sub_size, e));
      *dest = ret;
      return SUCCESS;
    }
    case T_UNION: {
      u16 sub_size;
      WRAP (union_t_get_serial_size (&sub_size, &t->un, e));
      WRAP (safe_add_u16 (&ret, sub_size, e));
      *dest = ret;
      return SUCCESS;
    }
    case T_SARRAY: {
      u16 sub_size;
      WRAP (sarray_t_get_serial_size (&sub_size, &t->sa, e));
      WRAP (safe_add_u16 (&ret, sub_size, e));
      *dest = ret;
      return SUCCESS;
    }
    default: {
      UNREACHABLE (); // LCOV_EXCL_LINE
      return 0;       // LCOV_EXCL_LINE
    }
  }
}

static void
prim_t_serialize (struct serializer *dest, const enum prim_t *src)
{
  DBG_ASSERT (prim_t, src);
  bool ret;
  (void)ret; // Unused in release

  // PRIM
  u8 prim_val = (u8)*src;
  ret         = srlizr_write (dest, (const u8 *)&prim_val, sizeof (u8));
  ASSERT (ret);
}

#ifndef NDEBUG
TEST (prim_t_serialize)
{
  enum prim_t       p = I16;
  u8                out[4];
  struct serializer s = srlizr_create (out, sizeof out);
  prim_t_serialize (&s, &p);

  u8 exp[] = {(u8)I16};
  test_assert_int_equal (s.dlen, sizeof exp);
  test_assert_int_equal (memcmp (out, exp, sizeof exp), 0);
}
#endif

void
type_serialize (struct serializer *dest, const struct type *src)
{
  DBG_ASSERT (valid_type, src);
  bool ret;
  (void)ret; // Unused in release

  u8 type_val = (u8)src->type;
  ret         = srlizr_write (dest, &type_val, sizeof (u8));
  ASSERT (ret);

  switch (src->type) {
    case T_PRIM: {
      prim_t_serialize (dest, &src->p);
      break;
    }
    case T_STRUCT: {
      struct_t_serialize (dest, &src->st);
      break;
    }
    case T_UNION: {
      union_t_serialize (dest, &src->un);
      break;
    }
    case T_SARRAY: {
      sarray_t_serialize (dest, &src->sa);
      break;
    }
    default: {
      UNREACHABLE (); // LCOV_EXCL_LINE
      break;          // LCOV_EXCL_LINE
    }
  }
}

/*-----------------------------------------------------------------------------
 * SUBSECTION: type_deserialize
 * @brief Deserialize a buffer into a type
 *----------------------------------------------------------------------------*/

static err_t
prim_t_deserialize (enum prim_t *dest, struct deserializer *src, error *e)
{
  ASSERT (dest);

  u8   p;
  bool ret = dsrlizr_read ((&p), sizeof (u8), src);
  if (!ret) {
    return error_causef (e, ERR_CORRUPT, "prim: missing length header");
  }

  enum prim_t _p = p;

  WRAP (prim_t_validate (&_p, e));

  DBG_ASSERT (prim_t, &_p);

  *dest = _p;

  return SUCCESS;
}

#ifndef NDEBUG
TEST (prim_t_deserialize)
{
  // 5.1 green path
  u8                  data[] = {(u8)CI32};
  struct deserializer d      = dsrlizr_create (data, sizeof data);
  enum prim_t         out    = 0;

  test_assert_int_equal (prim_t_deserialize (&out, &d, &ctx->e), SUCCESS);
  test_assert_int_equal (out, CI32);

  // 5.2 red path – invalid enum value (CU128+1)
  u8                  bad[] = {(u8)(CU128 + 1)};
  struct deserializer d2    = dsrlizr_create (bad, sizeof bad);
  test_assert_int_equal (prim_t_deserialize (&out, &d2, &ctx->e), ERR_INTERP);
  error_reset (&ctx->e);
}
#endif

struct type *
type_deserialize (struct deserializer *src, struct arena_alloc *alloc, error *e)
{
  u8           header;
  struct type *dest = arena_malloc (alloc, 1, sizeof *dest, e);
  if (dest == NULL) {
    return NULL;
  }
  // A short read returns false WITHOUT touching header, so reading it anyway
  // means reading uninitialized stack. That is zero on most platforms - and
  // T_PRIM is 0, a perfectly valid type - but MSVC's /RTC1 fills it with
  // 0xCC, which falls through to the default arm below. Hence Windows-only.
  if (!dsrlizr_read (&header, sizeof (u8), src)) {
    error_causef (e, ERR_CORRUPT, "Truncated type: no type code");
    return NULL;
  }

  dest->type = (enum type_t)header;

  switch (header) {
    case T_PRIM: {
      if (prim_t_deserialize (&dest->p, src, e)) {
        return NULL;
      }
      return dest;
    }
    case T_STRUCT: {
      if (struct_t_deserialize (&dest->st, src, alloc, e)) {
        return NULL;
      }
      return dest;
    }
    case T_UNION: {
      if (union_t_deserialize (&dest->un, src, alloc, e)) {
        return NULL;
      }
      return dest;
    }
    case T_SARRAY: {
      if (sarray_t_deserialize (&dest->sa, src, alloc, e)) {
        return NULL;
      }
      return dest;
    }
    default: {
      // Was reporting `ret` (the bool from the read) instead of the code.
      error_causef (e, ERR_INTERP, "Unknown type code: %d", (int)header);
      return NULL;
    }
  }
}

/*-----------------------------------------------------------------------------
 * SUBSECTION: type_random
 * @brief Generate a random type
 *----------------------------------------------------------------------------*/

static enum prim_t
prim_t_random (void)
{
  return (enum prim_t)randu32r (U8, CU128);
}

#ifndef NDEBUG
TEST (prim_t_random)
{
  for (u32 i = 0; i < 1000; ++i) {
    enum prim_t p = prim_t_random ();
    test_assert_int_equal (prim_t_validate (&p, &ctx->e), SUCCESS);
  }
}
#endif

static struct type *
type_random_once (struct arena_alloc *alloc, u32 depth, t_size max_tsize, error *e)
{
  struct type *dest = arena_malloc (alloc, 1, sizeof *dest, e);
  if (dest == NULL) {
    return NULL;
  }

  if (depth == 0) {
    dest->type = T_PRIM;
    dest->p    = prim_t_random ();
    return dest;
  }

  static const enum type_t weighted[] = {T_PRIM, T_STRUCT, T_UNION, T_SARRAY};

  dest->type                          = weighted[randu32r (0, arrlen (weighted) - 1)];

  switch (dest->type) {
    case T_PRIM: {
      dest->p = prim_t_random ();
      return dest;
    }

    case T_STRUCT: {
      if (struct_t_random (&dest->st, alloc, depth, max_tsize, e)) {
        return NULL;
      }
      return dest;
    }

    case T_UNION: {
      if (union_t_random (&dest->un, alloc, depth, max_tsize, e)) {
        return NULL;
      }
      return dest;
    }

    case T_SARRAY: {
      if (sarray_t_random (&dest->sa, alloc, depth, max_tsize, e)) {
        return NULL;
      }
      return dest;
    }

    default: {
      error_causef (e, ERR_NOMEM, "invalid type tag");
      return NULL;
    }
  }
  UNREACHABLE (); // LCOV_EXCL_LINE
}

struct type *
type_random (struct arena_alloc *alloc, u32 depth, t_size max_size, error *e)
{
  ALLOC_INIT (temp);

  struct type *ret = NULL;

  /**
   * Try a maximum of 100 times
   *  1. try to create a type
   *  2. if it's valid and within the size limit - return it
   *  3. otherwise, keep trying
   *
   * (100 is arbitrary - just an upperbound)
   */
  for (int i = 0; i < 100; ++i) {
    struct type *t = type_random_once (&temp, depth, max_size, e);

    if (t == NULL) {
      error_reset (e);
      continue;
    }

    // Random doesn't check string uniqueness
    // or any other valid type properties, so
    // random_once could return an invalid type
    // might be worth it to fix it, but not a big
    // deal
    if (type_validate (t, e) != SUCCESS) {
      error_reset (e);
      continue;
    }

    // Too wide, move on
    if (type_byte_size (t) > max_size) {
      continue;
    }

    ret = type_movemem (t, alloc, e);
    ALLOC_CLOSE (temp);
    return ret;
  }

  ALLOC_CLOSE (temp);

  error_causef (
      e,
      ERR_INVALID_ARGUMENT,
      "Failed to generate a valid type of at most %u bytes",
      max_size
  );
  return NULL;
}

#ifndef NDEBUG
TEST (type_random)
{
  int iters = 0;
  while (iters < 100) {
    t_size       max_tsize = randu32r (10, 4096);
    struct type *t         = type_random (&ctx->alloc, 5, max_tsize, &ctx->e);
    if (t != NULL) {
      iters++;
      test_assert (type_byte_size (t) <= max_tsize);
    }
  }

  arena_alloc_free_all (&ctx->alloc);
}
#endif

/*-----------------------------------------------------------------------------
 * SUBSECTION: type_equal
 * @brief Check if two types are equal
 *----------------------------------------------------------------------------*/

bool
type_equal (const struct type *left, const struct type *right)
{
  if (left->type != right->type) {
    return false;
  }

  switch (left->type) {
    case T_PRIM: {
      return left->p == right->p;
    }
    case T_STRUCT: {
      return struct_t_equal (&left->st, &right->st);
    }
    case T_UNION: {
      return union_t_equal (&left->un, &right->un);
    }
    case T_SARRAY: {
      return sarray_t_equal (&left->sa, &right->sa);
    }
    default: {
      UNREACHABLE (); // LCOV_EXCL_LINE
      return false;   // LCOV_EXCL_LINE
    }
  }
}

char *
get_var_str (struct type *t, u32 *dlen, error *e)
{
  i32 len = type_snprintf (NULL, 0, t);
  if (len < 0) {
    error_causef (e, ERR_IO, "snprintf failed");
    return NULL;
  }

  char *dest = (default_mem ()).table->malloc ((default_mem ()).self, len + 1, sizeof *dest, e);
  if (dest == NULL) {
    error_causef (e, ERR_NOMEM, "alloc failed for type log string");
    return NULL;
  }

  len = type_snprintf (dest, len + 1, t);
  if (len < 0) {
    (default_mem ()).table->free ((default_mem ()).self, dest);
    error_causef (e, ERR_IO, "snprintf failed");
    return NULL;
  }

  *dlen = len;

  return dest;
}

err_t
i_log_type (struct type *t, error *e)
{
  u32   len;
  char *var_str = get_var_str (t, &len, e);
  if (var_str == NULL) {
    return error_trace (e);
  }

  i_log_info ("%.*s\n", len, var_str);
  (default_mem ()).table->free ((default_mem ()).self, var_str);

  return SUCCESS;
}

static struct string
string_movemem (struct string src, struct arena_alloc *alloc, error *e)
{
  char *data = arena_alloc_copy (alloc, src.data, src.len, e);
  if (!data) {
    return (struct string){0};
  }
  return (struct string){.data = data, .len = src.len};
}

static struct string *
keylist_movemem (struct string *src, u32 len, struct arena_alloc *alloc, error *e)
{
  struct string *keys = arena_malloc (alloc, len, sizeof *keys, e);
  if (!keys) {
    return NULL;
  }

  for (u32 i = 0; i < len; ++i) {
    keys[i] = string_movemem (src[i], alloc, e);
    if (!keys[i].data) {
      return NULL;
    }
  }
  return keys;
}

static struct type **
typelist_movemem (struct type **src, u32 len, struct arena_alloc *alloc, error *e)
{
  struct type **types = arena_malloc (alloc, len, sizeof (struct type *), e);
  if (!types) {
    return NULL;
  }

  for (u32 i = 0; i < len; ++i) {
    types[i] = type_movemem (src[i], alloc, e);
    if (!types[i]) {
      return NULL;
    }
  }
  return types;
}

struct type *
type_movemem (struct type *src, struct arena_alloc *alloc, error *e)
{
  struct type *ret = arena_malloc (alloc, 1, sizeof *ret, e);
  if (!ret) {
    return NULL;
  }

  ret->type = src->type;

  switch (src->type) {
    case T_PRIM: {
      ret->p = src->p;
      break;
    }
    case T_STRUCT: {
      ret->st.len  = src->st.len;
      ret->st.keys = keylist_movemem (src->st.keys, src->st.len, alloc, e);
      if (!ret->st.keys) {
        return NULL;
      }
      ret->st.types = typelist_movemem (src->st.types, src->st.len, alloc, e);
      if (!ret->st.types) {
        return NULL;
      }
      break;
    }
    case T_UNION: {
      ret->un.len  = src->un.len;
      ret->un.keys = keylist_movemem (src->un.keys, src->un.len, alloc, e);
      if (!ret->un.keys) {
        return NULL;
      }
      ret->un.types = typelist_movemem (src->un.types, src->un.len, alloc, e);
      if (!ret->un.types) {
        return NULL;
      }
      break;
    }
    case T_SARRAY: {
      ret->sa.rank = src->sa.rank;
      ret->sa.t    = type_movemem (src->sa.t, alloc, e);
      if (ret->sa.t == NULL) {
        return NULL;
      }
      ret->sa.dims = arena_malloc (alloc, src->sa.rank, sizeof *ret->sa.dims, e);
      if (!ret->sa.dims) {
        return NULL;
      }
      memcpy (ret->sa.dims, src->sa.dims, src->sa.rank * sizeof *ret->sa.dims);
      break;
    }
  }

  return ret;
}

/******************************************************************************
 * SECTION: Primitive Types
 ******************************************************************************/

enum prim_t
strtoprim (const char *text, u32 len)
{
  struct string str = {.data = (char *)text, .len = len};

  if (string_equal (str, strfcstr ("u8"))) {
    return U8;
  }
  if (string_equal (str, strfcstr ("u16"))) {
    return U16;
  }
  if (string_equal (str, strfcstr ("u32"))) {
    return U32;
  }
  if (string_equal (str, strfcstr ("u64"))) {
    return U64;
  }
  if (string_equal (str, strfcstr ("i8"))) {
    return I8;
  }
  if (string_equal (str, strfcstr ("i16"))) {
    return I16;
  }
  if (string_equal (str, strfcstr ("i32"))) {
    return I32;
  }
  if (string_equal (str, strfcstr ("i64"))) {
    return I64;
  }
  if (string_equal (str, strfcstr ("f16"))) {
    return F16;
  }
  if (string_equal (str, strfcstr ("f32"))) {
    return F32;
  }
  if (string_equal (str, strfcstr ("f64"))) {
    return F64;
  }
  if (string_equal (str, strfcstr ("f128"))) {
    return F128;
  }
  if (string_equal (str, strfcstr ("cf32"))) {
    return CF32;
  }
  if (string_equal (str, strfcstr ("cf64"))) {
    return CF64;
  }
  if (string_equal (str, strfcstr ("cf128"))) {
    return CF128;
  }
  if (string_equal (str, strfcstr ("cf256"))) {
    return CF256;
  }
  if (string_equal (str, strfcstr ("ci16"))) {
    return CI16;
  }
  if (string_equal (str, strfcstr ("ci32"))) {
    return CI32;
  }
  if (string_equal (str, strfcstr ("ci64"))) {
    return CI64;
  }
  if (string_equal (str, strfcstr ("ci128"))) {
    return CI128;
  }
  if (string_equal (str, strfcstr ("cu16"))) {
    return CU16;
  }
  if (string_equal (str, strfcstr ("cu32"))) {
    return CU32;
  }
  if (string_equal (str, strfcstr ("cu64"))) {
    return CU64;
  }
  if (string_equal (str, strfcstr ("cu128"))) {
    return CU128;
  }

  return (enum prim_t) - 1;
}

/******************************************************************************
 * SECTION: Union
 * ----------------------------------------------------------------------------
 * @brief A Union type
 ******************************************************************************/

/******************************************************************************
 * SECTION: Print Type
 ******************************************************************************/

static void print_type_inner (
    int                level,
    const u8          *buf,
    const struct type *t,
    u32                max_elems,
    u32                indent
);

static void
print_indent (int level, u32 spaces)
{
  for (u32 i = 0; i < spaces; ++i) {
    i_log_printf (level, " ");
  }
}

#ifndef NDEBUG
TEST (print_indent)
{
  TEST_CASE ("smoke test")
  {
    print_indent (LOG_INFO, 4);
  }
}
#endif

static void
print_prim_value (int level, const u8 *buf, enum prim_t p)
{
  switch (p) {
    case U8: {
      u8 v;
      memcpy (&v, buf, 1);
      i_log_printf (level, "%u", (unsigned)v);
      return;
    }
    case U16: {
      u16 v;
      memcpy (&v, buf, 2);
      i_log_printf (level, "%u", (unsigned)v);
      return;
    }
    case U32: {
      u32 v;
      memcpy (&v, buf, 4);
      i_log_printf (level, "%u", (unsigned)v);
      return;
    }
    case U64: {
      u64 v;
      memcpy (&v, buf, 8);
      i_log_printf (level, "%lu", (unsigned long)v);
      return;
    }
    case I8: {
      i8 v;
      memcpy (&v, buf, 1);
      i_log_printf (level, "%d", (int)v);
      return;
    }
    case I16: {
      i16 v;
      memcpy (&v, buf, 2);
      i_log_printf (level, "%d", (int)v);
      return;
    }
    case I32: {
      i32 v;
      memcpy (&v, buf, 4);
      i_log_printf (level, "%d", (int)v);
      return;
    }
    case I64: {
      i64 v;
      memcpy (&v, buf, 8);
      i_log_printf (level, "%ld", (long)v);
      return;
    }
    case F16: {
      u16 h;
      memcpy (&h, buf, 2);
      i_log_printf (level, "%g", (double)f16_to_f32 (h));
      return;
    }
    case F32: {
      float v;
      memcpy (&v, buf, 4);
      i_log_printf (level, "%g", (double)v);
      return;
    }
    case F64: {
      double v;
      memcpy (&v, buf, 8);
      i_log_printf (level, "%g", v);
      return;
    }
    case F128: {
      u64 lo;
      u64 hi;
      memcpy (&lo, buf, 8);
      memcpy (&hi, buf + 8, 8);
      i_log_printf (level, "<f128:0x%016lx%016lx>", (unsigned long)hi, (unsigned long)lo);
      return;
    }
    case CF32: {
      u16 rh;
      u16 ih;
      memcpy (&rh, buf, 2);
      memcpy (&ih, buf + 2, 2);
      i_log_printf (level, "(%g, %g)", (double)f16_to_f32 (rh), (double)f16_to_f32 (ih));
      return;
    }
    case CF64: {
      float r;
      float im;
      memcpy (&r, buf, 4);
      memcpy (&im, buf + 4, 4);
      i_log_printf (level, "(%g, %g)", (double)r, (double)im);
      return;
    }
    case CF128: {
      double r;
      double im;
      memcpy (&r, buf, 8);
      memcpy (&im, buf + 8, 8);
      i_log_printf (level, "(%g, %g)", r, im);
      return;
    }
    case CF256: {
#if SIZEOF_LONG_DOUBLE >= 16
      long double r, im;
      memcpy (&r, buf, 16);
      memcpy (&im, buf + 16, 16);
      i_log_printf (level, "(%Lg, %Lg)", r, im);
#else
      u64 r_lo;
      u64 r_hi;
      u64 im_lo;
      u64 im_hi;
      memcpy (&r_lo, buf, 8);
      memcpy (&r_hi, buf + 8, 8);
      memcpy (&im_lo, buf + 16, 8);
      memcpy (&im_hi, buf + 24, 8);
      i_log_printf (
          level,
          "(<f128:0x%016lx%016lx>, "
          "<f128:0x%016lx%016lx>)",
          (unsigned long)r_hi,
          (unsigned long)r_lo,
          (unsigned long)im_hi,
          (unsigned long)im_lo
      );
#endif
      return;
    }
    case CI16: {
      i8 r;
      i8 im;
      memcpy (&r, buf, 1);
      memcpy (&im, buf + 1, 1);
      i_log_printf (level, "(%d, %d)", (int)r, (int)im);
      return;
    }
    case CI32: {
      i16 r;
      i16 im;
      memcpy (&r, buf, 2);
      memcpy (&im, buf + 2, 2);
      i_log_printf (level, "(%d, %d)", (int)r, (int)im);
      return;
    }
    case CI64: {
      i32 r;
      i32 im;
      memcpy (&r, buf, 4);
      memcpy (&im, buf + 4, 4);
      i_log_printf (level, "(%d, %d)", (int)r, (int)im);
      return;
    }
    case CI128: {
      i64 r;
      i64 im;
      memcpy (&r, buf, 8);
      memcpy (&im, buf + 8, 8);
      i_log_printf (level, "(%ld, %ld)", (long)r, (long)im);
      return;
    }
    case CU16: {
      u8 r;
      u8 im;
      memcpy (&r, buf, 1);
      memcpy (&im, buf + 1, 1);
      i_log_printf (level, "(%u, %u)", (unsigned)r, (unsigned)im);
      return;
    }
    case CU32: {
      u16 r;
      u16 im;
      memcpy (&r, buf, 2);
      memcpy (&im, buf + 2, 2);
      i_log_printf (level, "(%u, %u)", (unsigned)r, (unsigned)im);
      return;
    }
    case CU64: {
      u32 r;
      u32 im;
      memcpy (&r, buf, 4);
      memcpy (&im, buf + 4, 4);
      i_log_printf (level, "(%u, %u)", (unsigned)r, (unsigned)im);
      return;
    }
    case CU128: {
      u64 r;
      u64 im;
      memcpy (&r, buf, 8);
      memcpy (&im, buf + 8, 8);
      i_log_printf (level, "(%lu, %lu)", (unsigned long)r, (unsigned long)im);
      return;
    }
  }
}

#ifndef NDEBUG
TEST (print_prim_value)
{
  TEST_CASE ("smoke test")
  {
    u8 v = 10;
    print_prim_value (LOG_INFO, &v, U8);
  }
  TEST_CASE ("u8")
  {
    u8 v = 42;
    print_prim_value (LOG_INFO, (const u8 *)&v, U8);
  }
  TEST_CASE ("u16")
  {
    u16 v = 12345;
    print_prim_value (LOG_INFO, (const u8 *)&v, U16);
  }
  TEST_CASE ("u32")
  {
    u32 v = 1234567u;
    print_prim_value (LOG_INFO, (const u8 *)&v, U32);
  }
  TEST_CASE ("u64")
  {
    u64 v = 1234567890123ULL;
    print_prim_value (LOG_INFO, (const u8 *)&v, U64);
  }
  TEST_CASE ("i8")
  {
    i8 v = -42;
    print_prim_value (LOG_INFO, (const u8 *)&v, I8);
  }
  TEST_CASE ("i16")
  {
    i16 v = -12345;
    print_prim_value (LOG_INFO, (const u8 *)&v, I16);
  }
  TEST_CASE ("i32")
  {
    i32 v = -1234567;
    print_prim_value (LOG_INFO, (const u8 *)&v, I32);
  }
  TEST_CASE ("i64")
  {
    i64 v = -1234567890123LL;
    print_prim_value (LOG_INFO, (const u8 *)&v, I64);
  }
  TEST_CASE ("f16")
  {
    u16 v = 0x3C00; // 1.0 in IEEE 754 half-precision
    print_prim_value (LOG_INFO, (const u8 *)&v, F16);
  }
  TEST_CASE ("f32")
  {
    float v = 3.14f;
    print_prim_value (LOG_INFO, (const u8 *)&v, F32);
  }
  TEST_CASE ("f64")
  {
    double v = 3.14159265358979;
    print_prim_value (LOG_INFO, (const u8 *)&v, F64);
  }
  TEST_CASE ("f128")
  {
    u8 buf[16] = {0};
    print_prim_value (LOG_INFO, buf, F128);
  }
  TEST_CASE ("cf32")
  {
    u16 v[2] = {0x3C00, 0x4000}; // (1.0, 2.0) half-precision
    print_prim_value (LOG_INFO, (const u8 *)v, CF32);
  }
  TEST_CASE ("cf64")
  {
    float v[2] = {1.0f, 2.0f};
    print_prim_value (LOG_INFO, (const u8 *)v, CF64);
  }
  TEST_CASE ("cf128")
  {
    double v[2] = {1.0, 2.0};
    print_prim_value (LOG_INFO, (const u8 *)v, CF128);
  }
  TEST_CASE ("cf256")
  {
    u8 buf[32] = {0};
    print_prim_value (LOG_INFO, buf, CF256);
  }
  TEST_CASE ("ci16")
  {
    i8 v[2] = {-5, 5};
    print_prim_value (LOG_INFO, (const u8 *)v, CI16);
  }
  TEST_CASE ("ci32")
  {
    i16 v[2] = {-1000, 1000};
    print_prim_value (LOG_INFO, (const u8 *)v, CI32);
  }
  TEST_CASE ("ci64")
  {
    i32 v[2] = {-100000, 100000};
    print_prim_value (LOG_INFO, (const u8 *)v, CI64);
  }
  TEST_CASE ("ci128")
  {
    i64 v[2] = {-1000000000LL, 1000000000LL};
    print_prim_value (LOG_INFO, (const u8 *)v, CI128);
  }
  TEST_CASE ("cu16")
  {
    u8 v[2] = {5, 10};
    print_prim_value (LOG_INFO, (const u8 *)v, CU16);
  }
  TEST_CASE ("cu32")
  {
    u16 v[2] = {1000, 2000};
    print_prim_value (LOG_INFO, (const u8 *)v, CU32);
  }
  TEST_CASE ("cu64")
  {
    u32 v[2] = {100000u, 200000u};
    print_prim_value (LOG_INFO, (const u8 *)v, CU64);
  }
  TEST_CASE ("cu128")
  {
    u64 v[2] = {1000000000ULL, 2000000000ULL};
    print_prim_value (LOG_INFO, (const u8 *)v, CU128);
  }
}
#endif

// Product of dims[dim_idx+1 .. rank-1] * element_size
static u32
sarray_sub_size (const struct sarray_t *sa, u16 dim_idx)
{
  u32 sub = type_byte_size (sa->t);
  for (u16 i = dim_idx + 1; i < sa->rank; ++i) {
    sub *= sa->dims[i];
  }
  return sub;
}

#ifndef NDEBUG
TEST (sarray_sub_size)
{
  TEST_CASE ("smoke test")
  {
    struct type     element = {.type = T_PRIM, .p = I32};
    u32             dims[2] = {3, 4};
    struct sarray_t sa      = {.rank = 2, .dims = dims, .t = &element};
    u32             sub     = sarray_sub_size (&sa, 0);
    (void)sub;
  }
}
#endif

// col: visual column of the '[' just printed at this dimension,
// used to align continuation rows under it.
static void
print_sarray_dim (
    int                    level,
    const u8              *buf,
    const struct sarray_t *sa,
    u16                    dim_idx,
    u32                    max_elems,
    u32                    indent,
    u32                    col
)
{
  u32 dim_len  = sa->dims[dim_idx];
  u32 show     = dim_len < max_elems ? dim_len : max_elems;
  u32 sub_size = sarray_sub_size (sa, dim_idx);

  i_log_printf (level, "[");

  if (dim_idx == sa->rank - 1) {
    // Innermost dimension: elements inline
    for (u32 i = 0; i < show; ++i) {
      if (i > 0) {
        i_log_printf (level, ", ");
      }
      print_type_inner (level, buf + (i * sub_size), sa->t, max_elems, indent + 1);
    }
    if (dim_len > max_elems) {
      i_log_printf (level, ", ...");
    }
  } else {
    // Outer dimension: each sub-array on its own line
    for (u32 i = 0; i < show; ++i) {
      if (i > 0) {
        i_log_printf (level, ",\n");
        print_indent (level, col + 1);
      }
      print_sarray_dim (
          level,
          buf + (i * sub_size),
          sa,
          dim_idx + 1,
          max_elems,
          indent + 1,
          col + 1
      );
    }
    if (dim_len > max_elems) {
      i_log_printf (level, ",\n");
      print_indent (level, col + 1);
      i_log_printf (level, "...");
    }
  }

  i_log_printf (level, "]");
}

#ifndef NDEBUG
TEST (print_sarray_dim)
{
  TEST_CASE ("smoke test")
  {
    struct type     element = {.type = T_PRIM, .p = I32};
    u32             dims[1] = {3};
    struct sarray_t sa      = {.rank = 1, .dims = dims, .t = &element};
    i32             buf[3]  = {1, 2, 3};
    print_sarray_dim (LOG_INFO, (const u8 *)buf, &sa, 0, 10, 0, 0);
  }
}
#endif

static void
print_type_inner (int level, const u8 *buf, const struct type *t, u32 max_elems, u32 indent)
{
  switch (t->type) {
    case T_PRIM: {
      print_prim_value (level, buf, t->p);
      return;
    }

    case T_STRUCT: {
      i_log_printf (level, "{\n");
      u32 offset = 0;
      for (u16 i = 0; i < t->st.len; ++i) {
        u32 field_indent = indent + 4;
        print_indent (level, field_indent);
        i_log_printf (level, "%.*s = ", (int)t->st.keys[i].len, t->st.keys[i].data);

        const struct type *ft = t->st.types[i];
        if (ft->type == T_SARRAY) {
          u32 col = field_indent + t->st.keys[i].len + 3;
          print_sarray_dim (level, buf + offset, &ft->sa, 0, max_elems, field_indent, col);
        } else {
          print_type_inner (level, buf + offset, ft, max_elems, field_indent);
        }

        offset += type_byte_size (ft);
        if (i + 1 < t->st.len) {
          i_log_printf (level, ",");
        }
        i_log_printf (level, "\n");
      }
      print_indent (level, indent);
      i_log_printf (level, "}");
      return;
    }

    case T_UNION: {
      i_log_printf (level, "<union[0]: ");
      if (t->un.len > 0) {
        i_log_printf (level, "%.*s = ", (int)t->un.keys[0].len, t->un.keys[0].data);
        print_type_inner (level, buf, t->un.types[0], max_elems, indent);
      } else {
        i_log_printf (level, "empty");
      }
      i_log_printf (level, ">");
      return;
    }

    case T_SARRAY: {
      print_sarray_dim (level, buf, &t->sa, 0, max_elems, indent, indent);
      return;
    }
  }
}

#ifndef NDEBUG
TEST (print_type_inner)
{
  TEST_CASE ("smoke test")
  {
    struct type t = {.type = T_PRIM, .p = I32};
    i32         v = 42;
    print_type_inner (LOG_INFO, (const u8 *)&v, &t, 10, 0);
  }
}
#endif

void
type_print_data (int log_level, const u8 *buf, const struct type *t, u32 max_elems)
{
  print_type_inner (log_level, buf, t, max_elems, 0);
  i_log_printf (log_level, "\n");
}

#ifndef NDEBUG
TEST (type_print_data)
{
  TEST_CASE ("T_PRIM")
  {
    struct type t = {.type = T_PRIM, .p = I32};
    i32         v = -42;
    type_print_data (LOG_INFO, (const u8 *)&v, &t, 10);
  }
  TEST_CASE ("T_STRUCT")
  {
    struct string keys[2]  = {{.data = "x", .len = 1}, {.data = "y", .len = 1}};
    struct type   f1       = {.type = T_PRIM, .p = F32};
    struct type   f2       = {.type = T_PRIM, .p = F32};
    struct type  *types[2] = {&f1, &f2};
    struct type   t        = {.type = T_STRUCT, .st = {.len = 2, .keys = keys, .types = types}};
    float         buf[2]   = {1.5f, 2.5f};
    type_print_data (LOG_INFO, (const u8 *)buf, &t, 10);
  }
  TEST_CASE ("T_STRUCT with T_SARRAY field")
  {
    struct type   elem        = {.type = T_PRIM, .p = I32};
    u32           arr_dims[1] = {3};
    struct type   arr         = {.type = T_SARRAY, .sa = {.rank = 1, .dims = arr_dims, .t = &elem}};
    struct type   scalar      = {.type = T_PRIM, .p = U32};
    struct string keys[2]     = {{.data = "tag", .len = 3}, {.data = "values", .len = 6}};
    struct type  *types[2]    = {&scalar, &arr};
    struct type   t           = {.type = T_STRUCT, .st = {.len = 2, .keys = keys, .types = types}};
    u8            buf[16]     = {0};
    type_print_data (LOG_INFO, buf, &t, 10);
  }
  TEST_CASE ("T_UNION")
  {
    struct string keys[2]  = {{.data = "as_int", .len = 6}, {.data = "as_uint", .len = 7}};
    struct type   f1       = {.type = T_PRIM, .p = I64};
    struct type   f2       = {.type = T_PRIM, .p = U64};
    struct type  *types[2] = {&f1, &f2};
    struct type   t        = {.type = T_UNION, .un = {.len = 2, .keys = keys, .types = types}};
    i64           v        = -1;
    type_print_data (LOG_INFO, (const u8 *)&v, &t, 10);
  }
  TEST_CASE ("T_UNION empty")
  {
    struct type t   = {.type = T_UNION, .un = {.len = 0, .keys = NULL, .types = NULL}};
    u8          buf = 0;
    type_print_data (LOG_INFO, &buf, &t, 10);
  }
  TEST_CASE ("T_SARRAY 1d")
  {
    struct type element = {.type = T_PRIM, .p = I32};
    u32         dims[1] = {3};
    struct type t       = {.type = T_SARRAY, .sa = {.rank = 1, .dims = dims, .t = &element}};
    i32         buf[3]  = {1, 2, 3};
    type_print_data (LOG_INFO, (const u8 *)buf, &t, 10);
  }
  TEST_CASE ("T_SARRAY 1d truncated")
  {
    struct type element = {.type = T_PRIM, .p = I32};
    u32         dims[1] = {10};
    struct type t       = {.type = T_SARRAY, .sa = {.rank = 1, .dims = dims, .t = &element}};
    i32         buf[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    type_print_data (LOG_INFO, (const u8 *)buf, &t, 3);
  }
  TEST_CASE ("T_SARRAY 2d truncated")
  {
    struct type element = {.type = T_PRIM, .p = U8};
    u32         dims[2] = {5, 4};
    struct type t       = {.type = T_SARRAY, .sa = {.rank = 2, .dims = dims, .t = &element}};
    u8          buf[20] = {0};
    type_print_data (LOG_INFO, buf, &t, 2);
  }
}
#endif

struct type_printer_ostream_ctx
{
  struct type *t;
  t_size       pos;
  t_size       size;
  u8           buf[];
};

static i32
type_print_os_sink (struct stream *s, void *vctx, const void *src, u32 size, u32 n, error *e)
{
  (void)s;    // Unused
  (void)e;    // Unused
  (void)size; // Unused
  ASSERT (size == 1);
  struct type_printer_ostream_ctx *ctx   = (struct type_printer_ostream_ctx *)vctx;

  u32                              avail = ctx->size - ctx->pos;
  u32                              next  = MIN (avail, n);

  if (next == 0) {
    return 0;
  }

  memcpy (ctx->buf + ctx->pos, src, next);
  ctx->pos += next;

  if (ctx->pos == ctx->size) {
    type_print_data (LOG_INFO, ctx->buf, ctx->t, 3);
    ctx->pos = 0;
  }

  return (i32)next;
}

#ifndef NDEBUG
TEST (type_print_os_sink)
{
  TEST_CASE ("smoke test")
  {
    struct type t    = {.type = T_PRIM, .p = U32};
    t_size      size = type_byte_size (&t);
    struct type_printer_ostream_ctx
        *pctx       = ctx->mem.table->malloc (ctx->mem.self, 1, sizeof *pctx + size, &ctx->e);
    pctx->t         = &t;
    pctx->pos       = 0;
    pctx->size      = size;
    struct stream s = {0};
    u32           v = 0xDEADBEEF;
    type_print_os_sink (&s, pctx, &v, 1, sizeof v, &ctx->e);
    ctx->mem.table->free (ctx->mem.self, pctx);
  }
}
#endif

static void
type_print_os_close (void *ctx)
{
  (default_mem ()).table->free ((default_mem ()).self, (struct type_printer_ostream_ctx *)ctx);
}

#ifndef NDEBUG
TEST (type_print_os_close)
{
  TEST_CASE ("smoke test")
  {
    struct type_printer_ostream_ctx *pctx = ctx->mem.table
                                                ->malloc (ctx->mem.self, 1, sizeof *pctx, &ctx->e);
    type_print_os_close (pctx);
  }
}
#endif

static const struct stream_ops type_printer_os_ops = {
    .pull  = NULL,
    .push  = type_print_os_sink,
    .close = type_print_os_close,
};

err_t
type_stream_printer_init (struct stream *s, struct type *t, error *e)
{
  t_size size = type_byte_size (t);
  struct type_printer_ostream_ctx
      *ctx = (default_mem ()).table->malloc ((default_mem ()).self, 1, sizeof *ctx + size, e);
  if (ctx == NULL) {
    return error_trace (e);
  }

  ctx->size = size;
  ctx->pos  = 0;
  ctx->t    = t;
  stream_init (s, &type_printer_os_ops, ctx);

  return SUCCESS;
}
