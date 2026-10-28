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

#include "nscore/variables/ns_variables.h"

#include "core/ns_arena_alloc.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_logging.h"
#include "core/ns_numerics.h"
#include "core/ns_string.h"
#include "core/ns_testing.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"
#include "nscore/compiler/ns_compiler.h"
#include "nscore/compiler/ns_lexer.h"
#include "nscore/page/ns_page_var_hash_page.h"
#include "nscore/types/ns_types.h"

#include <string.h>

err_t
i_print_variable (struct variable *v, error *e)
{
  i_printf ("{\n");
  i_printf ("    \"Status\"          : \"Ok\",\n");
  i_printf ("    \"Name\"            : \"%.*s\",\n", strfmt (&v->vname));

  u32   len;
  char *var_str = get_var_str (v->dtype, &len, e);
  if (var_str == NULL) {
    return error_trace (e);
  }
  i_printf ("    \"DType\"           : \"%.*s\",\n", len, var_str);
  (default_mem ()).table->free ((default_mem ()).self, var_str);
  i_printf ("    \"DSize\"           : %u,\n", type_byte_size (v->dtype));

  /* Assuming Nelems is derived from bytes / size, or replace with v->nelems if
   * it exists */
  i_printf ("    \"Nelems\"          : %" PRb_size ",\n", v->nbytes / type_byte_size (v->dtype));
  i_printf ("    \"Bytes\"           : %" PRb_size ",\n", v->nbytes);

  /* JSON strictly requires lowercase 'null' */
  if (v->var_root == PGNO_NULL) {
    i_printf ("    \"Variable_Root\"   : NULL\n");
  } else {
    i_printf ("    \"Variable_Root\"   : %" PRpgno "\n", v->var_root);
  }

  /* JSON strictly requires lowercase 'null' */
  if (v->rpt_root == PGNO_NULL) {
    i_printf ("    \"Rope_Root\"       : NULL\n");
  } else {
    i_printf ("    \"Rope_Root\"       : %" PRpgno "\n", v->rpt_root);
  }

  i_printf ("}\n");

  return error_trace (e);
}

#ifndef NDEBUG
TEST (i_print_variable)
{
  struct variable v = {
      .vname    = strfcstr ("foo"),
      .dtype    = compile_type_alloc ("struct { a u32, b f32}", &ctx->alloc, &ctx->e),
      .var_root = 123,
      .rpt_root = 456,
      .nbytes   = 789,
  };

  // Just logging - make sure it succeeds and doesn't crash.
  test_assert (i_print_variable (&v, &ctx->e) == SUCCESS);

  arena_alloc_free_all (&ctx->alloc);
}
#endif

err_t
var_copy (struct variable *dest, struct variable *src, struct arena_alloc *alloc, error *e)
{
  *dest = (struct variable){
      .vname =
          {
              .data = arena_alloc_copy (alloc, src->vname.data, src->vname.len, e),
              .len  = src->vname.len,
          },
      // .dtype = copy
      .nbytes   = src->nbytes,
      .rpt_root = src->rpt_root,
      .var_root = src->var_root,
  };

  if (dest->vname.data == NULL) {
    return error_trace (e);
  }

  dest->dtype = type_movemem (src->dtype, alloc, e);
  if (dest->dtype == NULL) {
    return error_trace (e);
  }

  return SUCCESS;
}

#ifndef NDEBUG
TEST (var_copy)
{
  struct variable src = {
      .vname    = strfcstr ("foo"),
      .dtype    = compile_type_alloc ("struct { a u32, b f32}", &ctx->alloc, &ctx->e),
      .var_root = 123,
      .rpt_root = 456,
      .nbytes   = 789,
  };

  struct variable dest;
  test_assert (var_copy (&dest, &src, &ctx->alloc, &ctx->e) == SUCCESS);

  test_assert (variable_equal (&dest, &src));
  test_assert (dest.vname.data != src.vname.data);
  test_assert (dest.dtype != src.dtype);

  arena_alloc_free_all (&ctx->alloc);
}
#endif

bool
variable_equal (const struct variable *left, const struct variable *right)
{
  if (!string_equal (left->vname, right->vname)) {
    return false;
  }
  if (!type_equal (left->dtype, right->dtype)) {
    return false;
  }
  if (left->var_root != right->var_root) {
    return false;
  }
  if (left->rpt_root != right->rpt_root) {
    return false;
  }
  if (left->nbytes != right->nbytes) {
    return false;
  }

  return true;
}

#ifndef NDEBUG
TEST (variable_equal)
{
  struct variable a = {
      .vname    = strfcstr ("foo"),
      .dtype    = compile_type_alloc ("struct { a u32, b f32}", &ctx->alloc, &ctx->e),
      .var_root = 123,
      .rpt_root = 456,
      .nbytes   = 789,
  };

  struct variable b = {
      .vname    = strfcstr ("foo"),
      .dtype    = compile_type_alloc ("struct { a u32, b f32}", &ctx->alloc, &ctx->e),
      .var_root = 123,
      .rpt_root = 456,
      .nbytes   = 789,
  };

  test_assert (variable_equal (&a, &b));

  a.vname = strfcstr ("biz");
  test_assert (!variable_equal (&a, &b));
  a.vname = strfcstr ("foo");

  a.dtype = compile_type_alloc ("u32", &ctx->alloc, &ctx->e);
  test_assert (!variable_equal (&a, &b));
  a.dtype    = compile_type_alloc ("struct { a u32, b f32}", &ctx->alloc, &ctx->e);

  a.var_root = 987;
  test_assert (!variable_equal (&a, &b));
  a.var_root = 123;

  a.rpt_root = 999;
  test_assert (!variable_equal (&a, &b));
  a.rpt_root = 456;

  a.nbytes   = 111;
  test_assert (!variable_equal (&a, &b));
  a.nbytes = 789;

  arena_alloc_free_all (&ctx->alloc);
}
#endif

err_t
validate_vname (struct string vname, error *e)
{
  if (vname.len == 0) {
    return error_causef (e, ERR_INVALID_ARGUMENT, "variable name is empty");
  }

  if (vname.len >= 4096) {
    return error_causef (e, ERR_INVALID_ARGUMENT, "variable name exceeds 4096 chars");
  }

  if (!is_alpha (vname.data[0])) {
    return error_causef (
        e,
        ERR_INVALID_ARGUMENT,
        "variable name '%.*s' must start with a letter",
        vname.len,
        vname.data
    );
  }

  for (u32 i = 1; i < vname.len; ++i) {
    char c = vname.data[i];
    if (!is_alpha_num_generous (c)) {
      return error_causef (
          e,
          ERR_INVALID_ARGUMENT,
          "variable name '%.*s' contains "
          "invalid characters",
          vname.len,
          vname.data
      );
    }
  }

  return SUCCESS;
}

#ifndef NDEBUG
TEST (validate_vname)
{
  // Valid: plain letters.
  test_assert (validate_vname (strfcstr ("foo"), &ctx->e) == SUCCESS);

  // Valid: leading underscore, mixed digits/letters after.
  test_assert (validate_vname (strfcstr ("_valid_Name123"), &ctx->e) == SUCCESS);

  // Invalid: empty name.
  test_assert (validate_vname (strfcstr (""), &ctx->e) != SUCCESS);

  // Invalid: starts with a digit.
  test_assert (validate_vname (strfcstr ("1abc"), &ctx->e) != SUCCESS);

  // Invalid: contains a character outside the allowed set.
  test_assert (validate_vname (strfcstr ("abc!def"), &ctx->e) != SUCCESS);

  // Invalid: name is too long (>= 4096 chars).
  static char long_name[5000];
  memset (long_name, 'a', sizeof (long_name));
  struct string too_long = {.data = long_name, .len = sizeof (long_name)};
  test_assert (validate_vname (too_long, &ctx->e) != SUCCESS);
}
#endif

// Pool 1: Valid first characters (Letters and underscore)
const char alpha_pool[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "_";

// Pool 2: Valid remaining characters (Letters, digits, underscore, dot, slash,
// hyphen)
const char generous_pool[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "_";

void
var_random_name (char *buffer, u32 length)
{
  if (length == 0) {
    return;
  }

  int alpha_size    = sizeof (alpha_pool) - 1;
  int generous_size = sizeof (generous_pool) - 1;

  // A keyword or a primitive type name has a valid identifier shape but will
  // not parse as a variable name, so roll again until the name is usable
  do {
    // First char must strictly be an alpha or underscore
    buffer[0] = alpha_pool[randu32 () % alpha_size];

    // Remaining chars can use the generous pool
    for (u32 i = 1; i < length - 1; i++) {
      buffer[i] = generous_pool[randu32 () % generous_size];
    }
    buffer[length - 1] = '\0';
  }
  while (lex_is_keyword (buffer, length - 1));
}

#ifndef NDEBUG

static bool
test_char_in_pool (char c, const char *pool, int pool_size)
{
  for (int i = 0; i < pool_size; i++) {
    if (pool[i] == c) {
      return true;
    }
  }
  return false;
}

TEST (var_random_name)
{
  // First char is always from alpha_pool, and the last is always the
  // null terminator, with everything in between from generous_pool.
  for (int trial = 0; trial < 10; trial++) {
    char buf[16];
    var_random_name (buf, sizeof (buf));

    test_assert (test_char_in_pool (buf[0], alpha_pool, sizeof (alpha_pool) - 1));
    test_assert_int_equal ((int)buf[sizeof (buf) - 1], '\0');

    for (u32 i = 1; i < sizeof (buf) - 1; i++) {
      test_assert (test_char_in_pool (buf[i], generous_pool, sizeof (generous_pool) - 1));
    }
  }

  // length <= 0 should leave the buffer untouched.
  char sentinel[4] = {'x', 'x', 'x', 'x'};
  var_random_name (sentinel, 0);
  test_assert (memcmp (sentinel, "xxxx", 4) == 0);

  // NOTE: with length == 1, the null terminator write at
  // buffer[length - 1] overwrites the alpha char written moments
  // before, so the "name" ends up being just '\0'.
  char single[1];
  var_random_name (single, 1);
  test_assert_int_equal ((int)single[0], '\0');

  // A generated name must never come back as a keyword or a primitive type
  // name. Such a name has a valid identifier shape but does not parse as one,
  // so `create if u32` fails to compile -- ns_simtest hit exactly that with
  // the names "if" and "i8".
  //
  // Only short names can collide (the longest reserved word is 6 chars), so
  // roll the short lengths hard rather than many lengths lightly.
  for (u32 length = 3; length <= 8; length++) {
    for (int trial = 0; trial < 10000; trial++) {
      char buf[9];
      var_random_name (buf, length);
      test_assert (!lex_is_keyword (buf, length - 1));
    }
  }
}

#endif // TESTING

err_t
rand_varname (
    struct string      *dest,
    struct arena_alloc *alloc,
    const u32           minlen,
    const u32           maxlen,
    error              *e
)
{
  ASSERT (dest);
  ASSERT (alloc);
  ASSERT (minlen > 0);
  ASSERT (minlen <= maxlen);

  u32   len    = randu32r (minlen, maxlen);
  char *buffer = arena_malloc (alloc, len, 1, e);
  if (buffer == NULL) {
    return error_trace (e);
  }
  var_random_name (buffer, len);

  dest->data = buffer;
  dest->len  = len;

  return SUCCESS;
}

#ifndef NDEBUG
TEST (rand_varname)
{
  for (int i = 0; i < 10; ++i) {
    struct string name;
    err_t         ret = rand_varname (&name, &ctx->alloc, 5, 10, &ctx->e);

    test_assert_int_equal (ret, SUCCESS);
    test_assert (name.len >= 5 && name.len <= 10);
    test_assert (test_char_in_pool (name.data[0], alpha_pool, sizeof (alpha_pool) - 1));
    test_assert_int_equal (name.data[name.len - 1], '\0');
  }

  arena_alloc_free_all (&ctx->alloc);
}
#endif

err_t
rand_varname_same_hash (
    struct string      *name1,
    struct string      *name2,
    struct arena_alloc *alloc,
    error              *e
)
{
  ASSERT (name1);
  ASSERT (name2);
  ASSERT (alloc);

  char temp[20];

  while (true) {
    // Random sizes
    u32 len1 = randu32r (5, 10);
    u32 len2 = randu32r (5, 10);

    // Random names
    var_random_name (temp, len1);
    var_random_name (temp + len1, len2);

    // Assign them
    name1->data  = temp;
    name2->data  = temp + len1;
    name1->len   = len1;
    name2->len   = len2;

    // Get hash positions
    p_size hpos1 = vh_get_hash_pos (*name1);
    p_size hpos2 = vh_get_hash_pos (*name2);

    // Check if they are good
    if (hpos1 == hpos2) {
      // commit strings - copy them to dest
      char *data = arena_malloc (alloc, len1 + len2, 1, e);
      if (data == NULL) {
        goto failed;
      }
      memcpy (data, name1->data, len1);
      memcpy (data + len1, name2->data, len2);
      name1->data = data;
      name2->data = data + len1;
      break;
    }
  }

  return SUCCESS;

failed:
  return error_trace (e);
}

err_t
rand_varname_different_hash (
    struct string      *name1,
    struct string      *name2,
    struct arena_alloc *alloc,
    error              *e
)
{
  ASSERT (name1);
  ASSERT (name2);
  ASSERT (alloc);

  char temp[20];

  while (true) {
    // Random sizes
    u32 len1 = randu32r (5, 10);
    u32 len2 = randu32r (5, 10);

    // Random names
    var_random_name (temp, len1);
    var_random_name (temp + len1, len2);

    // Assign them
    name1->data  = temp;
    name2->data  = temp + len1;
    name1->len   = len1;
    name2->len   = len2;

    // Get hash positions
    p_size hpos1 = vh_get_hash_pos (*name1);
    p_size hpos2 = vh_get_hash_pos (*name2);

    // Check if they are good
    if (hpos1 != hpos2) {
      // commit strings - copy them to dest
      char *data = arena_malloc (alloc, len1 + len2, 1, e);
      if (data == NULL) {
        goto failed;
      }
      memcpy (data, name1->data, len1);
      memcpy (data + len1, name2->data, len2);
      name1->data = data;
      name2->data = data + len1;
      break;
    }
  }

  return SUCCESS;

failed:
  return error_trace (e);
}

#ifndef NDEBUG
TEST (rand_varname_same_hash)
{
  for (int i = 0; i < 10; ++i) {
    struct string name1;
    struct string name2;
    rand_varname_same_hash (&name1, &name2, &ctx->alloc, &ctx->e);
    test_assert_int_equal (vh_get_hash_pos (name1), vh_get_hash_pos (name2));
  }

  arena_alloc_free_all (&ctx->alloc);
}

TEST (rand_varname_different_hash)
{
  for (int i = 0; i < 10; ++i) {
    struct string name1;
    struct string name2;
    rand_varname_different_hash (&name1, &name2, &ctx->alloc, &ctx->e);
    test_assert (vh_get_hash_pos (name1) != vh_get_hash_pos (name2));
  }

  arena_alloc_free_all (&ctx->alloc);
}
#endif

b_size
var_resolve_index (struct variable *v, sb_size bofst)
{
  // Translate negative
  if (bofst < 0) {
    bofst = v->nbytes + bofst;
  }

  // was so negative it's still negative after conversion
  if (bofst < 0) {
    bofst = 0;
  }

  // Translate indexes past nybtes
  if ((b_size)bofst > v->nbytes) // also: > not >=, so nbytes itself is valid (append)
  {
    bofst = v->nbytes;
  }

  return bofst;
}

#ifndef NDEBUG
TEST (var_resolve_index)
{
  struct variable v = {.nbytes = 100};

  // Positive offset within range is unchanged.
  test_assert_int_equal (var_resolve_index (&v, 10), 10);

  // Offset exactly at nbytes is valid (append position).
  test_assert_int_equal (var_resolve_index (&v, 100), 100);

  // Offset past nbytes is clamped to nbytes.
  test_assert_int_equal (var_resolve_index (&v, 150), 100);

  // Offset of 0 is unchanged.
  test_assert_int_equal (var_resolve_index (&v, 0), 0);

  // Negative offset is translated relative to nbytes.
  test_assert_int_equal (var_resolve_index (&v, -10), 90);

  // Negative offset equal to -nbytes translates to 0.
  test_assert_int_equal (var_resolve_index (&v, -100), 0);

  // Negative offset more negative than -nbytes clamps to 0.
  test_assert_int_equal (var_resolve_index (&v, -150), 0);
}
#endif

b_size
var_resolve_nelem (struct variable *v, b_size bofst, b_size nelem, t_size size)
{
  b_size remainder = (v->nbytes - bofst) / size;
  if (nelem > remainder) {
    nelem = remainder;
  }
  return nelem;
}

#ifndef NDEBUG
TEST (var_resolve_nelem)
{
  struct variable v = {.nbytes = 100};

  // nelem within the available remainder is unchanged.
  test_assert_int_equal (var_resolve_nelem (&v, 0, 10, 1), 10);

  // nelem exactly at the remainder is unchanged.
  test_assert_int_equal (var_resolve_nelem (&v, 0, 100, 1), 100);

  // nelem past the remainder is clamped down to it.
  test_assert_int_equal (var_resolve_nelem (&v, 0, 200, 1), 100);

  // Remainder shrinks as bofst moves forward.
  test_assert_int_equal (var_resolve_nelem (&v, 90, 20, 1), 10);

  // Larger element size reduces the available element count
  // (integer division truncates any partial trailing element).
  test_assert_int_equal (var_resolve_nelem (&v, 0, 100, 8), 12);

  // bofst == nbytes leaves no remainder at all.
  test_assert_int_equal (var_resolve_nelem (&v, 100, 5, 1), 0);
}
#endif

err_t
variable_copy (
    struct variable       *dest,
    const struct variable *src,
    struct arena_alloc    *alloc,
    error                 *e
)
{
  // Copy over the variable name
  // TODO - this is awful - remove +1
  dest->vname.data = arena_alloc_copy (alloc, src->vname.data, src->vname.len + 1, e);
  dest->dtype      = type_movemem (src->dtype, alloc, e);
  if (dest->vname.data == NULL) {
    return error_trace (e);
  }
  if (dest->dtype == NULL) {
    return error_trace (e);
  }

  dest->vname.len = src->vname.len;
  dest->var_root  = src->var_root;
  dest->rpt_root  = src->rpt_root;
  dest->nbytes    = src->nbytes;

  return SUCCESS;
}

struct string
vname_or_default (const char *name)
{
  if (name != NULL) {
    return strfcstr (name);
  } else {
    return strfcstr (DEFAULT_VARIABLE);
  }
}

#ifndef NDEBUG
TEST (vname_or_default)
{
  test_assert (string_equal (vname_or_default ("foo"), strfcstr ("foo")));
  test_assert (string_equal (vname_or_default (NULL), strfcstr (DEFAULT_VARIABLE)));
}
#endif
