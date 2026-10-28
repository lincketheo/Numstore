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

#ifndef NS_PAGE_INNER_NODE_H
#define NS_PAGE_INNER_NODE_H

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_platform.h"
#include "core/ns_stdtypes.h"
#include "nscore/page/ns_page.h"

#include <stdbool.h>
#include <string.h>

/******************************************************************************
 * SECTION: Inner Node
 ******************************************************************************/

struct in_pair
{
  pgno   pg;
  b_size key; // number of bytes covered by this child's subtree
};

#define in_pair_from(_pg, _key) \
  (struct in_pair)              \
  {                             \
    .pg = (_pg), .key = (_key), \
  }

#define in_pair_is_empty(o) ((o).pg == PGNO_NULL)

struct three_in_pair
{
  struct in_pair prev;
  struct in_pair cur;
  struct in_pair next;
};

#define in_pair_empty \
  (struct in_pair)    \
  {                   \
    .pg = PGNO_NULL   \
  }

DEFINE_DBG_ASSERT (page, inner_node, i, { ASSERT (i); })

struct in_data
{
  struct in_pair *nodes;
  u32             len;
};

#define IN_NEXT_OFST PG_COMMN_END
#define IN_PREV_OFST ((p_size)(IN_NEXT_OFST + sizeof (pgno)))
#define IN_NLEN_OFST ((p_size)(IN_PREV_OFST + sizeof (pgno)))
#define IN_LEAF_OFST ((p_size)(IN_NLEN_OFST + sizeof (p_size)))

_Static_assert (
    NS_PAGE_SIZE > IN_LEAF_OFST + (5 * sizeof (b_size)) + (6 * sizeof (pgno)),
    "Inner Node: NS_PAGE_SIZE must be > IN_LEAF_OFST plus at least 5 keys"
);

#define IN_MAX_KEYS (p_size) ((NS_PAGE_SIZE - IN_LEAF_OFST) / (sizeof (pgno) + sizeof (b_size)))
#define IN_MIN_KEYS (IN_MAX_KEYS / 2)

_Static_assert (IN_MAX_KEYS > 5, "Inner Node: IN_MAX_KEYS must be > 5");

void in_init_empty (page *in);
err_t in_validate_for_db (const page *in, error *e);
p_size in_page_memcpy_right (pgno *dest, const page *src, p_size ofst);
p_size in_key_memcpy_right (b_size *dest, const page *src, p_size ofst);
void in_push_left (page *in, p_size len);
void in_push_left_permissive (page *in, p_size len);
void in_cut_left (page *in, p_size end);
void in_set_data (page *p, struct in_data data);
void in_move_left (page *dest, page *src, p_size len);
void in_move_right (page *src, page *dest, p_size len);
void in_choose_lidx (p_size *idx, b_size *nleft, const page *node, b_size loc);
void i_log_in (int level, const page *in);
void in_make_valid (page *in);

////////////////////////////////////////////////////////////
// GETTERS

HEADER_FUNC p_size
in_get_len (const page *in)
{
  PAGE_SIMPLE_GET_IMPL (in, p_size, IN_NLEN_OFST);
}

HEADER_FUNC const void *
in_get_backwards_keys_imut (const page *in)
{
  const p_size n      = in_get_len (in);
  const p_size nbytes = n * sizeof (b_size);
  ASSERT (nbytes <= NS_PAGE_SIZE);
  if (nbytes > NS_PAGE_SIZE) {
    UNREACHABLE_HINT (); // invariant: callers guarantee nbytes fits in page
  }

  if (nbytes == 0) {
    return NULL;
  }

  return &in->raw[NS_PAGE_SIZE - nbytes];
}

HEADER_FUNC b_size
in_get_key (const page *in, const p_size idx)
{
  const p_size n = in_get_len (in);
  ASSERT (idx < n);

  const u8    *base         = in_get_backwards_keys_imut (in);
  const p_size offset_elems = n - 1 - idx;
  const p_size offset_bytes = offset_elems * sizeof (b_size);

  b_size       ret;
  memcpy (&ret, base + offset_bytes, sizeof ret);

  return ret;
}

HEADER_FUNC b_size
in_get_size (const page *in)
{
  b_size ret = 0;
  // TODO - (17) this could be cached

  for (p_size i = 0; i < in_get_len (in); ++i) {
    ret += in_get_key (in, i);
  }

  return ret;
}

HEADER_FUNC p_size
in_get_avail (const page *in)
{
  return IN_MAX_KEYS - in_get_len (in);
}

HEADER_FUNC void *
in_get_backwards_keys_mut (page *in)
{
  const p_size n      = in_get_len (in);
  const p_size nbytes = n * sizeof (b_size);
  ASSERT (nbytes <= NS_PAGE_SIZE);

  return &in->raw[NS_PAGE_SIZE - nbytes];
}

HEADER_FUNC void *
in_get_backwards_keys_mut_limit (page *in)
{
  const p_size nbytes = IN_MAX_KEYS * sizeof (b_size);
  ASSERT (nbytes <= NS_PAGE_SIZE);
  return &in->raw[NS_PAGE_SIZE - nbytes];
}

HEADER_FUNC void *
in_get_leafs_mut (page *in)
{
  return &in->raw[IN_LEAF_OFST];
}

HEADER_FUNC const void *
in_get_leafs_imut (const page *in)
{
  return &in->raw[IN_LEAF_OFST];
}

#define in_get_right_most_key(in) in_get_key (in, in_get_len (in) - 1)

HEADER_FUNC pgno
in_get_leaf (const page *in, const p_size idx)
{
  const p_size n = in_get_len (in);
  (void)n; // Unused in release
  ASSERT (idx < n);

  pgno      leaf;

  const u8 *head = in_get_leafs_imut (in);
  memcpy (&leaf, head + (idx * sizeof (pgno)), sizeof (leaf));

  return leaf;
}

HEADER_FUNC pgno
in_get_next (const page *in)
{
  PAGE_SIMPLE_GET_IMPL (in, pgno, IN_NEXT_OFST);
}

HEADER_FUNC pgno
in_get_prev (const page *in)
{
  PAGE_SIMPLE_GET_IMPL (in, pgno, IN_PREV_OFST);
}

HEADER_FUNC bool
in_is_root (const page *in)
{
  DBG_ASSERT (inner_node, in);
  return (in_get_next (in) == PGNO_NULL && in_get_prev (in) == PGNO_NULL) != 0;
}

// Shorthands
#define in_get_right_most_key(in)  in_get_key (in, in_get_len (in) - 1)
#define in_get_right_most_leaf(in) in_get_leaf (in, in_get_len (in) - 1)
#define in_get_nchildren(in)       (in_get_len (in) + 1)
#define in_get_first_leaf(in)      in_get_leaf (in, 0)
#define in_get_last_leaf(in)       in_get_leaf (in, in_get_len (in) - 1)
#define in_full(in)                (in_get_len (in) == IN_MAX_KEYS)

struct inner_node
{
  struct in_pair pairs[IN_MAX_KEYS];
  p_size         plen;
  p_size         avail;
  bool           full;
  pgno           npg;
  pgno           ppg;
  bool           is_root;
};

HEADER_FUNC void
in_get (struct inner_node *dest, const page *p)
{
  dest->plen    = in_get_len (p);
  dest->avail   = in_get_avail (p);
  dest->full    = in_full (p);
  dest->npg     = in_get_next (p);
  dest->ppg     = in_get_prev (p);
  dest->is_root = in_is_root (p);

  for (p_size i = 0; i < dest->plen; ++i) {
    dest->pairs[i].key = in_get_key (p, i);
    dest->pairs[i].pg  = in_get_leaf (p, i);
  }
}

////////////////////////////////////////////////////////////
// SETTERS

HEADER_FUNC void
in_set_len (page *in, const p_size len)
{
  PAGE_SIMPLE_SET_IMPL (in, len, IN_NLEN_OFST);
}

HEADER_FUNC void
in_set_leaf (page *in, const p_size idx, const pgno pg)
{
  ASSERT (idx < in_get_len (in) + 1);
  PAGE_SIMPLE_SET_IMPL (in, pg, IN_LEAF_OFST + idx * sizeof (pgno));
}

HEADER_FUNC void
in_set_key (page *in, const p_size idx, const b_size key)
{
  const p_size n = in_get_len (in);
  ASSERT (idx < n);

  u8          *base         = in_get_backwards_keys_mut (in);
  const p_size offset_elems = n - 1 - idx;
  const p_size offset_bytes = offset_elems * sizeof (b_size);

  memcpy (base + offset_bytes, &key, sizeof key);
}

HEADER_FUNC void
in_set_key_leaf (page *in, const p_size idx, const b_size key, const pgno pg)
{
  in_set_key (in, idx, key);
  in_set_leaf (in, idx, pg);
}

HEADER_FUNC void
in_push_end (page *dest, const b_size key, const pgno pg)
{
  ASSERT (!in_full (dest));
  in_set_len (dest, in_get_len (dest) + 1);
  in_set_key_leaf (dest, in_get_len (dest) - 1, key, pg);
}

HEADER_FUNC void
in_set_leaf_ignore_len (page *in, const p_size idx, const pgno pg)
{
  ASSERT (idx < IN_MAX_KEYS);
  PAGE_SIMPLE_SET_IMPL (in, pg, IN_LEAF_OFST + idx * sizeof (pgno));
}

HEADER_FUNC void
in_set_key_ignore_len (page *in, const p_size idx, const b_size key)
{
  ASSERT (idx < IN_MAX_KEYS);

  u8          *base         = in_get_backwards_keys_mut (in);
  const p_size offset_elems = IN_MAX_KEYS - 1 - idx;
  const p_size offset_bytes = offset_elems * sizeof (b_size);

  memcpy (base + offset_bytes, &key, sizeof key);
}

HEADER_FUNC void
in_set_key_leaf_ignore_len (page *in, const p_size idx, const b_size key, const pgno pg)
{
  in_set_key_ignore_len (in, idx, key);
  in_set_leaf_ignore_len (in, idx, pg);
}

HEADER_FUNC void
in_set_prev (page *in, const pgno prev)
{
  PAGE_SIMPLE_SET_IMPL (in, prev, IN_PREV_OFST);
}

HEADER_FUNC void
in_set_next (page *in, const pgno next)
{
  PAGE_SIMPLE_SET_IMPL (in, next, IN_NEXT_OFST);
}

HEADER_FUNC void
in_link (page *left, page *right)
{
  const pgno lpg = left ? left->pg : PGNO_NULL;
  const pgno rpg = right ? right->pg : PGNO_NULL;
  if (left) {
    in_set_next (left, rpg);
  }
  if (right) {
    in_set_prev (right, lpg);
  }
}

#endif
