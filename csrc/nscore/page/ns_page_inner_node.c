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

#include "nscore/page/ns_page_inner_node.h"

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_htable.h"
#include "core/ns_numerics.h"
#include "core/ns_stdtypes.h"
#include "core/ns_testing.h"
#include "nscore/page/ns_page.h"

#include <stdio.h>

/******************************************************************************
 * SECTION: Inner Node
 ******************************************************************************/

#ifndef NDEBUG
static void
inner_node_init_for_testing (page *in, const pgno *pgs, const b_size *keys, const p_size len)
{
  page_init_empty (in, PG_INNER_NODE);
  in_set_len (in, 0);

  for (p_size i = 0; i < len; ++i) {
    in_push_end (in, keys[i], pgs[i]);
  }
}

static void
in_print (const page *in)
{
  for (p_size i = 0; i < in_get_len (in); ++i) {
    const pgno   pg  = in_get_leaf (in, i);
    const b_size key = in_get_key (in, i);
    printf ("(%" PRpgno " %" PRb_size "), ", pg, key);
  }
  printf ("\n");
}

static void
in_print_as_arrays (const pgno *pgs, const b_size *keys, const p_size len)
{
  for (p_size i = 0; i < len; ++i) {
    const pgno   pg  = pgs[i];
    const b_size key = keys[i];
    printf ("(%" PRpgno " %" PRb_size "), ", pg, key);
  }
  printf ("\n");
}

static void
test_assert_inner_node_equal (
    const page   *actual,
    const pgno   *e_pgs,
    const b_size *e_keys,
    const p_size  len
)
{
  test_assert_int_equal (len, in_get_len (actual));

  for (p_size i = 0; i < len; ++i) {
    const pgno   pg  = in_get_leaf (actual, i);
    const b_size key = in_get_key (actual, i);

    if (pg != e_pgs[i] || e_keys[i] != key) {
      i_log_failure ("Inner Nodes were not equal:\n");
      i_log_failure ("Expected:\n");
      in_print_as_arrays (e_pgs, e_keys, len);
      i_log_failure ("Actual:\n");
      in_print (actual);
    }

    test_assert_type_equal (pg, e_pgs[i], pgno, PRpgno);
    test_assert_type_equal (key, e_keys[i], b_size, PRb_size);
  }
}

/// in_print and in_print_as_arrays only ever run from the failure path of
/// test_assert_inner_node_equal, so a green run never reaches them and a red
/// one is the worst time to find out one of them walks off the end of a node.
/// This exercises both over the shapes that path can hand them.
TEST (in_print)
{
  page in;

  TEST_CASE ("empty node prints nothing but the newline")
  {
    inner_node_init_for_testing (&in, NULL, NULL, 0);

    test_assert_int_equal (in_get_len (&in), 0);

    in_print (&in);
    in_print_as_arrays (NULL, NULL, 0);
  }

  TEST_CASE ("populated node and its arrays print the same pairs")
  {
    const pgno   pgs[3]  = {100, 101, 102};
    const b_size keys[3] = {5, 7, 9};

    inner_node_init_for_testing (&in, pgs, keys, 3);

    in_print (&in);
    in_print_as_arrays (pgs, keys, 3);

    // Both printers read the same pairs out of their own source - the thing
    // that makes the two dumps comparable by eye when a test fails
    for (p_size i = 0; i < 3; ++i) {
      test_assert_type_equal (in_get_leaf (&in, i), pgs[i], pgno, PRpgno);
      test_assert_type_equal (in_get_key (&in, i), keys[i], b_size, PRb_size);
    }

    // And the pair of them is exactly what a passing comparison asserts
    test_assert_inner_node_equal (&in, pgs, keys, 3);
  }

  TEST_CASE ("full node")
  {
    // The failure path dumps whatever len says, so the widest node a caller
    // can hand it has to be safe too
    static pgno   pgs[IN_MAX_KEYS];
    static b_size keys[IN_MAX_KEYS];
    for (p_size i = 0; i < IN_MAX_KEYS; ++i) {
      pgs[i]  = (pgno)(i + 1);
      keys[i] = (b_size)(i * 2);
    }

    inner_node_init_for_testing (&in, pgs, keys, IN_MAX_KEYS);

    test_assert_int_equal (in_get_len (&in), IN_MAX_KEYS);
    test_assert (in_full (&in));

    in_print (&in);
    in_print_as_arrays (pgs, keys, IN_MAX_KEYS);

    test_assert_inner_node_equal (&in, pgs, keys, IN_MAX_KEYS);
  }
}
#endif

#define VTYPE  int
#define KTYPE  pgno
#define SUFFIX pgno
#include "core/ns_robin_hood_ht.h"

#undef VTYPE
#undef KTYPE
#undef SUFFIX

// Initialization
void
in_init_empty (page *in)
{
  ASSERT (page_get_type (in) == PG_INNER_NODE);
  in_set_next (in, PGNO_NULL);
  in_set_prev (in, PGNO_NULL);
  in_set_len (in, 0);
  DBG_ASSERT (inner_node, in);
}

err_t
in_validate_for_db (const page *in, error *e)
{
  const pgh header = page_get_type (in);

  // Has the correct header
  if (header != (pgh)PG_INNER_NODE) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "expected header: %" PRpgh " but got: %" PRpgh,
        (pgh)PG_INNER_NODE,
        (pgh)header
    );
  }

  // Len is less than Max Keys
  if (in_get_len (in) > IN_MAX_KEYS) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "inner node len %" PRp_size " > max %" PRp_size " (page_size=%" PRp_size ")",
        in_get_len (in),
        IN_MAX_KEYS,
        NS_PAGE_SIZE
    );
  }

  // Check for duplicates
  hash_table_pgno ht;
  hentry_pgno     data[IN_MAX_KEYS];
  ht_init_pgno (&ht, data, IN_MAX_KEYS);

  for (p_size i = 0; i < in_get_len (in); ++i) {
    const hdata_pgno _data = {
        .key   = in_get_leaf (in, i),
        .value = 0,
    };
    const hti_res res = ht_insert_pgno (&ht, _data);
    if (res != HTIR_SUCCESS) {
      return error_causef (e, ERR_CORRUPT, "duplicate leaf %" PRpgno " in inner node", _data.key);
    }
  }

  if (in_is_root (in)) {
    if (in_get_len (in) == 0) {
      return error_causef (
          e,
          ERR_CORRUPT,
          "Root node must have at least 1 "
          "element"
      );
    }
  } else {
    if (in_get_len (in) < IN_MAX_KEYS / 2) {
      return error_causef (
          e,
          ERR_CORRUPT,
          "non-root inner node below half "
          "capacity (len=%" PRp_size " min=%" PRp_size ")",
          in_get_len (in),
          IN_MAX_KEYS / 2
      );
    }
  }

  return SUCCESS;
}

#ifndef NDEBUG
TEST (in_validate_for_db)
{
  page in;

  TEST_CASE ("Invalid header")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 2);
    in_set_key_leaf (&in, 0, 1, 2);
    in_set_key_leaf (&in, 1, 2, 3);
    page_set_type (&in, PG_DATA_LIST);
    test_assert_int_equal (in_validate_for_db (&in, &ctx->e), ERR_CORRUPT);
    error_reset (&ctx->e);
  }

  TEST_CASE ("Len is too large")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 2);
    in_set_key_leaf (&in, 0, 1, 2);
    in_set_key_leaf (&in, 1, 2, 3);
    in_set_len (&in, IN_MAX_KEYS + 4);
    test_assert_int_equal (in_validate_for_db (&in, &ctx->e), ERR_CORRUPT);
    error_reset (&ctx->e);
  }

  TEST_CASE ("Keys are not strict monotonic (ok - used to be bad)")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 2);
    in_set_key_leaf (&in, 0, 2, 2);
    in_set_key_leaf (&in, 1, 1, 3);
    test_assert_int_equal (in_validate_for_db (&in, &ctx->e), SUCCESS);
    error_reset (&ctx->e);
  }

  TEST_CASE ("Duplicate leafs")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 2);
    in_set_key_leaf (&in, 0, 2, 2);
    in_set_key_leaf (&in, 1, 5, 2);
    test_assert_int_equal (in_validate_for_db (&in, &ctx->e), ERR_CORRUPT);
    error_reset (&ctx->e);
  }

  TEST_CASE ("Green path")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 2);
    in_set_key_leaf (&in, 0, 2, 2);
    in_set_key_leaf (&in, 1, 5, 3);
    test_assert_int_equal (in_validate_for_db (&in, &ctx->e), SUCCESS);
  }
}
#endif

// Getters

#ifndef NDEBUG
TEST (in_set_get_simple)
{
  page p;
  rand_bytes (p.raw, NS_PAGE_SIZE);

  TEST_CASE ("Start: freshly initialized page")
  {
    page_init_empty (&p, PG_INNER_NODE);

    test_assert_int_equal (in_get_len (&p), 0);
    test_assert_int_equal (in_get_size (&p), 0);
    test_assert_int_equal (in_get_avail (&p), IN_MAX_KEYS);
    test_assert_type_equal (in_get_next (&p), PGNO_NULL, pgno, PRpgno);
    test_assert_type_equal (in_get_prev (&p), PGNO_NULL, pgno, PRpgno);
  }

  TEST_CASE ("Set / Get basics")
  {
    page_init_empty (&p, PG_INNER_NODE);

    // Set next/prev pointers
    in_set_next (&p, 42);
    in_set_prev (&p, 43);
    test_assert_type_equal (in_get_next (&p), 42, pgno, PRpgno);
    test_assert_type_equal (in_get_prev (&p), 43, pgno, PRpgno);

    // Push a few keys/leaves
    in_push_end (&p, 5, 100);
    in_push_end (&p, 7, 101);
    in_push_end (&p, 9, 102);

    // Length & capacity
    test_assert_int_equal (in_get_len (&p), 3);
    test_assert_int_equal (in_get_avail (&p), IN_MAX_KEYS - 3);

    // Keys and unravelled sizes
    test_assert_int_equal (in_get_key (&p, 0), 5);
    test_assert_int_equal (in_get_key (&p, 1), 7);
    test_assert_int_equal (in_get_key (&p, 2), 9);

    // Leaves
    test_assert_type_equal (in_get_leaf (&p, 0), 100, pgno, PRpgno);
    test_assert_type_equal (in_get_leaf (&p, 1), 101, pgno, PRpgno);
    test_assert_type_equal (in_get_leaf (&p, 2), 102, pgno, PRpgno);

    // Derived macros
    test_assert_int_equal (in_get_right_most_key (&p), 9);
    test_assert_type_equal (in_get_right_most_leaf (&p), 102, pgno, PRpgno);
    test_assert_int_equal (in_get_nchildren (&p), 4);
    test_assert (in_full (&p) == false);
    test_assert_type_equal (in_get_first_leaf (&p), 100, pgno, PRpgno);
    test_assert_type_equal (in_get_last_leaf (&p), 102, pgno, PRpgno);
  }
}
#endif

// Setters
#ifndef NDEBUG
TEST (in_push_end)
{
  page in;

  TEST_CASE ("Happy path - filled")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 2);
    in_set_key_leaf (&in, 0, 1, 2);
    in_set_key_leaf (&in, 1, 2, 3);
    in_push_end (&in, 5, 6);

    test_assert_int_equal (in_get_len (&in), 3);
    test_assert_int_equal (in_get_leaf (&in, 2), 6);
    test_assert_int_equal (in_get_key (&in, 2), 5);
  }

  TEST_CASE ("Happy path - empty")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 0);
    in_push_end (&in, 5, 6);

    test_assert_int_equal (in_get_len (&in), 1);
    test_assert_int_equal (in_get_leaf (&in, 0), 6);
    test_assert_int_equal (in_get_key (&in, 0), 5);
  }
}
#endif

p_size
in_page_memcpy_right (pgno *dest, const page *src, const p_size ofst)
{
  ASSERT (ofst <= in_get_len (src));
  if (ofst == in_get_len (src)) {
    return 0;
  }

  const p_size moving = in_get_len (src) - ofst;
  const pgno  *head   = in_get_leafs_imut (src);

  memcpy (dest, head + ofst, moving * sizeof *dest);
  return moving;
}

p_size
in_key_memcpy_right (b_size *dest, const page *src, const p_size ofst)
{
  ASSERT (ofst <= in_get_len (src));

  if (ofst == in_get_len (src)) {
    return 0;
  }

  for (u32 i = ofst; i < in_get_len (src); ++i) {
    dest[i - ofst] = in_get_key (src, i);
  }

  return in_get_len (src) - ofst;
}

#ifndef NDEBUG
TEST (in_memcpy)
{
  page   in;

  pgno   dest_page[NS_PAGE_SIZE];
  b_size dest_key[NS_PAGE_SIZE];

  TEST_CASE ("Size 0")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 1);
    in_set_key_leaf (&in, 0, 1, 2);

    // Page
    p_size copied = in_page_memcpy_right (dest_page, &in, 1);
    test_assert_int_equal (copied, 0);

    // key
    copied = in_key_memcpy_right (dest_key, &in, 1);
    test_assert_int_equal (copied, 0);
  }

  TEST_CASE ("Size 1")
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 1);
    in_set_key_leaf (&in, 0, 1, 2);

    // Page
    p_size copied = in_page_memcpy_right (dest_page, &in, 0);
    test_assert_int_equal (copied, 1);
    test_assert_int_equal (dest_page[0], 2);
    memset (dest_page, 0xFF, sizeof (dest_page));

    // key
    copied = in_key_memcpy_right (dest_key, &in, 0);
    test_assert_int_equal (copied, 1);
    test_assert_int_equal (dest_key[0], 1);
    memset (dest_key, 0xFF, sizeof (dest_key));
  }

  // Size > 1
  {
    page_init_empty (&in, PG_INNER_NODE);
    in_set_len (&in, 0);
    in_push_end (&in, 5, 0);
    in_push_end (&in, 6, 1);
    in_push_end (&in, 3, 2);
    in_push_end (&in, 1, 3);
    in_push_end (&in, 2, 4);
    in_push_end (&in, 10, 5);
    in_push_end (&in, 13, 6);
    in_push_end (&in, 8, 7);
    in_push_end (&in, 11, 8);
    in_push_end (&in, 12, 9);

    // Page
    const pgno expected_p[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    p_size     copied       = in_page_memcpy_right (dest_page, &in, 0);
    test_assert_int_equal (copied, 10);
    test_assert_int_equal (memcmp (dest_page, expected_p, sizeof (expected_p)), 0);
    memset (dest_page, 0xFF, sizeof (dest_page));
    copied = in_page_memcpy_right (dest_page, &in, 3);
    test_assert_int_equal (copied, 7);
    test_assert_int_equal (
        memcmp (dest_page, &expected_p[3], sizeof (expected_p) - 3 * sizeof *expected_p),
        0
    );
    memset (dest_page, 0xFF, sizeof (dest_page));

    // key
    const b_size expected_b1[] = {5, 6, 3, 1, 2, 10, 13, 8, 11, 12};
    copied                     = in_key_memcpy_right (dest_key, &in, 0);
    test_assert_int_equal (copied, 10);
    test_assert_int_equal (memcmp (dest_key, expected_b1, sizeof (expected_b1)), 0);
    memset (dest_key, 0xFF, sizeof (dest_key));
    copied = in_key_memcpy_right (dest_key, &in, 3);
    test_assert_int_equal (copied, 7);
    test_assert_int_equal (
        memcmp (dest_key, &expected_b1[3], sizeof (expected_b1) - 3 * sizeof *expected_b1),
        0
    );
    memset (dest_key, 0xFF, sizeof (dest_key));
  }
}
#endif

void
in_set_data (page *p, const struct in_data data)
{
  ASSERT (data.len <= IN_MAX_KEYS);

  in_set_len (p, 0);

  for (p_size i = 0; i < data.len; ++i) {
    in_push_end (p, data.nodes[i].key, data.nodes[i].pg);
  }
}

void
in_move_left (page *dest, page *src, const p_size len)
{
  ASSERT (len <= in_get_len (src));
  ASSERT (len <= in_get_avail (dest));

  for (p_size i = 0; i < len; ++i) {
    const pgno   pg  = in_get_leaf (src, i);
    const b_size key = in_get_key (src, i);
    in_push_end (dest, key, pg);
  }

  in_cut_left (src, len);
}

#ifndef NDEBUG
TEST (in_move_left)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, (pgno[]){0, 1, 2, 3}, (b_size[]){10, 20, 30, 40}, 4);

  inner_node_init_for_testing (&right, (pgno[]){4, 5, 6, 7}, (b_size[]){5, 11, 18, 26}, 4);

  in_move_left (&left, &right, 1);

  test_assert_inner_node_equal (&left, (pgno[]){0, 1, 2, 3, 4}, (b_size[]){10, 20, 30, 40, 5}, 5);

  test_assert_inner_node_equal (&right, (pgno[]){5, 6, 7}, (b_size[]){11, 18, 26}, 3);
}

#  ifndef NDEBUG
TEST (in_move_left_two_keys)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, (pgno[]){10}, (b_size[]){15}, 1);

  inner_node_init_for_testing (&right, (pgno[]){20, 21, 22}, (b_size[]){5, 13, 22}, 3);

  in_move_left (&left, &right, 2);

  test_assert_inner_node_equal (&left, (pgno[]){10, 20, 21}, (b_size[]){15, 5, 13}, 3);

  test_assert_inner_node_equal (&right, (pgno[]){22}, (b_size[]){22}, 1);
}
#  endif

#  ifndef NDEBUG
TEST (in_move_left_all_keys)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, (pgno[]){1, 2}, (b_size[]){12, 28}, 2);

  inner_node_init_for_testing (&right, (pgno[]){3, 4}, (b_size[]){5, 10}, 2);

  in_move_left (&left, &right, 2);

  test_assert_inner_node_equal (&left, (pgno[]){1, 2, 3, 4}, (b_size[]){12, 28, 5, 10}, 4);

  test_assert_inner_node_equal (&right, NULL, NULL, 0);
}
#  endif

#  ifndef NDEBUG
TEST (in_move_left_into_empty)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, NULL, NULL, 0);

  inner_node_init_for_testing (&right, (pgno[]){5, 6, 7}, (b_size[]){4, 10, 19}, 3);

  in_move_left (&left, &right, 2);

  test_assert_inner_node_equal (&left, (pgno[]){5, 6}, (b_size[]){4, 10}, 2);

  test_assert_inner_node_equal (&right, (pgno[]){7}, (b_size[]){19}, 1);
}
#  endif
#endif

void
in_push_left (page *in, const p_size len)
{
  ASSERT (len + in_get_len (in) <= IN_MAX_KEYS);

  if (len == 0) {
    return;
  }

  const p_size len0 = in_get_len (in);
  in_set_len (in, len + in_get_len (in));

  for (p_size i = len0; i > 0; --i) {
    const pgno   pg  = in_get_leaf (in, i - 1);
    const b_size key = in_get_key (in, i - 1);
    in_set_key_leaf (in, i + len - 1, key, pg);
  }
}

void
in_push_left_permissive (page *in, const p_size amnt)
{
  ASSERT (in_get_len (in) == IN_MAX_KEYS);
  ASSERT (amnt <= IN_MAX_KEYS);

  for (p_size i = amnt, k = IN_MAX_KEYS; i > 0; --i, --k) {
    const pgno   pg  = in_get_leaf (in, i - 1);
    const b_size key = in_get_key (in, i - 1);
    in_set_key_leaf (in, k - 1, key, pg);
  }
}

#ifndef NDEBUG
TEST (in_push_left)
{
  page in;

  inner_node_init_for_testing (&in, (pgno[]){0, 1, 2, 3}, (b_size[]){10, 21, 33, 46}, 4);

  in_push_left (&in, 1);

  test_assert_inner_node_equal (&in, (pgno[]){0, 0, 1, 2, 3}, (b_size[]){10, 10, 21, 33, 46}, 5);

  in_push_left (&in, 2);

  test_assert_inner_node_equal (
      &in,
      (pgno[]){0, 0, 0, 0, 1, 2, 3},
      (b_size[]){10, 10, 10, 10, 21, 33, 46},
      7
  );
}

#  ifndef NDEBUG
TEST (in_push_left_into_empty)
{
  page in;

  inner_node_init_for_testing (&in, NULL, NULL, 0);

  in_push_left (&in, 2);

  test_assert_int_equal (in_get_len (&in), 2);
}
#  endif

#  ifndef NDEBUG
TEST (in_push_left_to_full)
{
  page   in;

  // Start with IN_MAX_KEYS - 1 keys
  b_size keys[IN_MAX_KEYS - 1];
  pgno   pages[IN_MAX_KEYS - 1];

  for (u32 i = 0; i < IN_MAX_KEYS - 1; ++i) {
    keys[i]  = i;
    pages[i] = i;
  }

  inner_node_init_for_testing (&in, pages, keys, IN_MAX_KEYS - 1);

  in_push_left (&in, 1);

  b_size new_keys[IN_MAX_KEYS];
  pgno   new_pages[IN_MAX_KEYS];
  new_keys[0]  = 0;
  new_pages[0] = 0;
  for (u32 i = 1; i < IN_MAX_KEYS; ++i) {
    new_keys[i]  = i - 1;
    new_pages[i] = i - 1;
  }

  test_assert_inner_node_equal (&in, new_pages, new_keys, IN_MAX_KEYS);
}
#  endif

#endif

void
in_move_right (page *src, page *dest, const p_size len)
{
  ASSERT (len <= in_get_len (src));
  ASSERT (len <= in_get_avail (dest));

  if (len == 0) {
    return;
  }

  in_push_left (dest, len);

  b_size key = 0;
  for (p_size di = 0, si = in_get_len (src) - len; di < len; ++di, ++si) {
    const pgno pg = in_get_leaf (src, si);
    key           = in_get_key (src, si);

    in_set_key_leaf (dest, di, key, pg);
  }

  in_set_len (src, in_get_len (src) - len);
}

#ifndef NDEBUG
TEST (in_move_right)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, (pgno[]){0, 1, 2, 3}, (b_size[]){10, 20, 30, 40}, 4);

  inner_node_init_for_testing (&right, (pgno[]){4, 5, 6, 7}, (b_size[]){5, 11, 18, 26}, 4);

  in_move_right (&left, &right, 1);

  test_assert_inner_node_equal (&left, (pgno[]){0, 1, 2}, (b_size[]){10, 20, 30}, 3);

  test_assert_inner_node_equal (&right, (pgno[]){3, 4, 5, 6, 7}, (b_size[]){40, 5, 11, 18, 26}, 5);
}

#  ifndef NDEBUG
TEST (in_move_right_two_keys)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, (pgno[]){0, 1, 2, 3}, (b_size[]){10, 20, 30, 40}, 4);

  inner_node_init_for_testing (&right, (pgno[]){4, 5}, (b_size[]){5, 11}, 2);

  in_move_right (&left, &right, 2);

  test_assert_inner_node_equal (&left, (pgno[]){0, 1}, (b_size[]){10, 20}, 2);

  test_assert_inner_node_equal (&right, (pgno[]){2, 3, 4, 5}, (b_size[]){30, 40, 5, 11}, 4);
}
#  endif

#  ifndef NDEBUG
TEST (in_move_right_all_keys)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, (pgno[]){0, 1}, (b_size[]){10, 25}, 2);

  inner_node_init_for_testing (&right, (pgno[]){2}, (b_size[]){5}, 1);

  in_move_right (&left, &right, 2);

  test_assert_inner_node_equal (&left, NULL, NULL, 0);

  test_assert_inner_node_equal (&right, (pgno[]){0, 1, 2}, (b_size[]){10, 25, 5}, 3);
}
#  endif

#  ifndef NDEBUG
TEST (in_move_right_into_empty_right)
{
  page left;
  page right;

  inner_node_init_for_testing (&left, (pgno[]){42, 43, 44}, (b_size[]){7, 15, 28}, 3);

  inner_node_init_for_testing (&right, NULL, NULL, 0); // empty page

  in_move_right (&left, &right, 1);

  test_assert_inner_node_equal (&left, (pgno[]){42, 43}, (b_size[]){7, 15}, 2);

  test_assert_inner_node_equal (&right, (pgno[]){44}, (b_size[]){28},
                                1); // 28 - 15 = 13 unravel
}
#  endif

#endif

// Utils
void
in_choose_lidx (p_size *idx, b_size *nleft, const page *node, const b_size loc)
{
  const u32 n = in_get_len (node);
  ASSERT (n != 0);
  b_size key_total = 0;
  *nleft           = 0;

  p_size i         = 0;
  for (; i < n - 1; ++i) {
    const b_size key = in_get_key (node, i);
    key_total += key;

    if (loc < key_total) {
      *idx = i;
      return;
    }

    *nleft += key;
  }

  *idx = i;
}

#ifndef NDEBUG
TEST (in_choose_lidx)
{
  page in;

  page_init_empty (&in, PG_INNER_NODE);
  in_set_len (&in, 5);
  // 2 4 5 10 11
  in_set_key_leaf (&in, 0, 2, 10);
  in_set_key_leaf (&in, 1, 2, 11);
  in_set_key_leaf (&in, 2, 1, 12);
  in_set_key_leaf (&in, 3, 5, 13);
  in_set_key_leaf (&in, 4, 1, 14);
  p_size idx;
  b_size nleft;
  in_choose_lidx (&idx, &nleft, &in, 0);
  test_assert_int_equal (idx, 0);
  test_assert_int_equal (nleft, 0);

  in_choose_lidx (&idx, &nleft, &in, 1);
  test_assert_int_equal (idx, 0);
  test_assert_int_equal (nleft, 0);

  in_choose_lidx (&idx, &nleft, &in, 2);
  test_assert_int_equal (idx, 1);
  test_assert_int_equal (nleft, 2);

  in_choose_lidx (&idx, &nleft, &in, 3);
  test_assert_int_equal (idx, 1);
  test_assert_int_equal (nleft, 2);

  in_choose_lidx (&idx, &nleft, &in, 4);
  test_assert_int_equal (idx, 2);
  test_assert_int_equal (nleft, 4);

  in_choose_lidx (&idx, &nleft, &in, 5);
  test_assert_int_equal (idx, 3);
  test_assert_int_equal (nleft, 5);

  in_choose_lidx (&idx, &nleft, &in, 6);
  test_assert_int_equal (idx, 3);
  test_assert_int_equal (nleft, 5);

  in_choose_lidx (&idx, &nleft, &in, 7);
  test_assert_int_equal (idx, 3);
  test_assert_int_equal (nleft, 5);

  in_choose_lidx (&idx, &nleft, &in, 8);
  test_assert_int_equal (idx, 3);
  test_assert_int_equal (nleft, 5);

  in_choose_lidx (&idx, &nleft, &in, 9);
  test_assert_int_equal (idx, 3);
  test_assert_int_equal (nleft, 5);

  in_choose_lidx (&idx, &nleft, &in, 10);
  test_assert_int_equal (idx, 4);
  test_assert_int_equal (nleft, 10);

  in_choose_lidx (&idx, &nleft, &in, 11);
  test_assert_int_equal (idx, 4);
  test_assert_int_equal (nleft, 10);

  in_choose_lidx (&idx, &nleft, &in, 12);
  test_assert_int_equal (idx, 4);
  test_assert_int_equal (nleft, 10);

  in_choose_lidx (&idx, &nleft, &in, 13);
  test_assert_int_equal (idx, 4);
  test_assert_int_equal (nleft, 10);

  in_choose_lidx (&idx, &nleft, &in, 5000);
  test_assert_int_equal (idx, 4);
  test_assert_int_equal (nleft, 10);
}
#endif

void
in_cut_left (page *in, const p_size end)
{
  ASSERT (end <= in_get_len (in));

  if (end == 0) {
    return;
  }

  for (p_size i = 0; i < in_get_len (in) - end; ++i) {
    const pgno   pg  = in_get_leaf (in, end + i);
    const b_size key = in_get_key (in, end + i);

    in_set_key_leaf (in, i, key, pg);
  }

  in_set_len (in, in_get_len (in) - end);
}

#ifndef NDEBUG
TEST (in_cut_left)
{
  page in;

  inner_node_init_for_testing (&in, (pgno[]){0, 1, 2, 3}, (b_size[]){10, 21, 33, 46}, 4);

  in_cut_left (&in, 0);

  test_assert_inner_node_equal (&in, (pgno[]){0, 1, 2, 3}, (b_size[]){10, 21, 33, 46}, 4);

  in_cut_left (&in, 1);

  test_assert_inner_node_equal (&in, (pgno[]){1, 2, 3}, (b_size[]){21, 33, 46}, 3);

  in_cut_left (&in, 2);

  test_assert_inner_node_equal (&in, (pgno[]){3}, (b_size[]){46}, 1);

  in_cut_left (&in, 1);

  test_assert_inner_node_equal (&in, NULL, NULL, 0);
}

TEST (in_cut_left_all_at_once)
{
  page in;

  inner_node_init_for_testing (&in, (pgno[]){10, 20, 30}, (b_size[]){5, 15, 30}, 3);

  in_cut_left (&in, 3);

  test_assert_inner_node_equal (&in, NULL, NULL, 0);
}
#endif

#ifndef NDEBUG
TEST (in_cut_left_from_empty)
{
  page in;

  inner_node_init_for_testing (&in, NULL, NULL, 0);

  in_cut_left (&in, 0);

  test_assert_inner_node_equal (&in, NULL, NULL, 0);
}
#endif

#ifndef NDEBUG
TEST (in_cut_left_to_one)
{
  page in;

  inner_node_init_for_testing (&in, (pgno[]){1, 2, 3, 4}, (b_size[]){7, 14, 22, 31}, 4);

  in_cut_left (&in, 3);

  test_assert_inner_node_equal (&in, (pgno[]){4}, (b_size[]){31}, 1);
}
#endif

void
in_make_valid (page *d)
{
  in_set_len (d, IN_MAX_KEYS);
  for (p_size i = 0; i < IN_MAX_KEYS; ++i) {
    in_set_key_leaf (d, i, i, i);
  }
}

void
i_log_in (const int level, const page *in)
{
  i_log (level, "=== INNER NODE PAGE START ===\n");

  i_log_printf (level, "PGNO: %" PRpgno "\n", in->pg);
  if (in_get_next (in) == PGNO_NULL) {
    i_log_printf (level, "NEXT: NULL\n");
  } else {
    i_log_printf (level, "NEXT: %" PRpgno "\n", in_get_next (in));
  }
  if (in_get_prev (in) == PGNO_NULL) {
    i_log_printf (level, "PREV: NULL\n");
  } else {
    i_log_printf (level, "PREV: %" PRpgno "\n", in_get_prev (in));
  }
  i_log_printf (level, "SIZE: %" PRb_size "\n", in_get_size (in));
  i_log_printf (level, "LEN:  %u\n", in_get_len (in));

  const u32 len = in_get_len (in);
  for (u32 i = 0; i < len; ++i) {
    char line[128] = {0};
    int  pos       = 0;
    pos += snprintf (
        &line[pos],
        sizeof (line) - pos,
        "(%" PRb_size ", %" PRpgno ")",
        in_get_key (in, i),
        in_get_leaf (in, i)
    );
    i_log_printf (level, "%s ", line);
  }
  i_log_printf (level, "\n");

  i_log (level, "=== INNER NODE PAGE END ===\n");
}

#ifndef NDEBUG
TEST (i_log_in)
{
  page in;
  in.pg = 10;

  page_init_empty (&in, PG_INNER_NODE);
  in_make_valid (&in);
  in_set_prev (&in, 10);
  in_set_next (&in, 10);
  i_log_in (LOG_INFO, &in);

  in_set_prev (&in, PGNO_NULL);
  in_set_next (&in, PGNO_NULL);
  i_log_in (LOG_INFO, &in);
}
#endif
