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

#include "nscore/lock_table/ns_lock_table.h"

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_gr_lock.h"
#include "core/ns_htable.h"
#include "core/ns_utils.h"
#include "nscore/txn_table/ns_txn_table.h"

#include <string.h>

/**
 * Serialize the data within a lock into a
 * single u32
 */
static u32
lt_lock_key (const struct lt_lock lock)
{
  // The containing buffer
  char     hcode[sizeof (u8)];
  u32      hcodelen = 0;
  const u8 _type    = lock.type;

  memcpy (&hcode[hcodelen], &_type, sizeof (_type));
  hcodelen += sizeof (_type);

  switch (lock.type) {
    case LOCK_DB: {
      break;
    }
  }

  const struct string lock_type_hcode = {
      .data = hcode,
      .len  = hcodelen,
  };

  return fnv1a_hash (lock_type_hcode);
}

/**
 * Get the parent lock (heirarchial locking)
 */
static bool
get_parent (struct lt_lock *parent, const struct lt_lock lock)
{
  (void)parent; // Unused for now
  switch (lock.type) {
    case LOCK_DB: {
      return false;
    }
  }

  UNREACHABLE (); // LCOV_EXCL_LINE
}

/**
 * Holder of a lock
 */
struct lockt_frame
{
  struct lt_lock key;  // The lock key
  struct gr_lock lock; // The actual lock
  struct hnode   node; // Intrusive hash node
  int            refcount;
};

static err_t
lockt_frame_init (
    struct lockt_frame  *dest,
    const struct lt_lock key,
    const struct i_os    os,
    error               *e
)
{
  if (gr_lock_init (&dest->lock, os, e) < 0) {
    return error_trace (e);
  }

  dest->key      = key;
  dest->refcount = 0;
  hnode_init (&dest->node, lt_lock_key (key));

  return SUCCESS;
}

static void
lockt_frame_destroy (struct lockt_frame *dest)
{
  gr_lock_destroy (&dest->lock);
}

static void
lockt_frame_init_key (struct lockt_frame *dest, const struct lt_lock key)
{
  dest->key = key;
  hnode_init (&dest->node, lt_lock_key (key));
}

static bool
lockt_frame_eq (const struct hnode *left, const struct hnode *right)
{
  const struct lockt_frame *_left  = container_of (left, struct lockt_frame, node);
  const struct lockt_frame *_right = container_of (right, struct lockt_frame, node);
  return lt_lock_equal (_left->key, _right->key);
}

static void
frame_ref (struct lockt_frame *frame)
{
  frame->refcount++;
}

static void
frame_unref_unsafe (struct lockt *t, struct lockt_frame *frame)
{
  ASSERT (frame->refcount > 0);
  frame->refcount--;

  if (frame->refcount == 0) {
    struct hnode **found = htable_lookup (t->table, &frame->node, lockt_frame_eq);
    htable_delete (t->table, found);
    lockt_frame_destroy (frame);
    slab_alloc_free (&t->lock_alloc, frame);
  }
}

err_t
lockt_init (struct lockt *t, struct i_mem mem, const struct i_os os, error *e)
{
  t->os = os;
  slab_alloc_init (&t->lock_alloc, mem, sizeof (struct lockt_frame), 1000);

  t->table = htable_create (1000, mem, e);
  if (t->table == NULL) {
    return error_trace (e);
  }

  latch_init (&t->l);

  return SUCCESS;
}

void
lockt_destroy (struct lockt *t)
{
  slab_alloc_destroy (&t->lock_alloc);
  htable_free (t->table);
}

static struct lockt_frame *
lockt_new_frame_unsafe (struct lockt *t, struct lt_lock lock, error *e)
{
  // Allocate and insert a new frame
  struct lockt_frame *frame = slab_alloc_alloc (&t->lock_alloc, e);
  if (e->cause_code != SUCCESS) {
    return NULL;
  }

  if (lockt_frame_init (frame, lock, t->os, e) != SUCCESS) {
    slab_alloc_free (&t->lock_alloc, frame);
    return NULL;
  }

  htable_insert (t->table, &frame->node);

  return frame;
}

static struct lockt_frame *
get_or_create_frame_unsafe (struct lockt *t, struct lt_lock lock, error *e)
{
  // Create the lookup key
  struct lockt_frame lookup_key;
  lockt_frame_init_key (&lookup_key, lock);

  struct hnode      **node  = htable_lookup (t->table, &lookup_key.node, lockt_frame_eq);

  // This is the frame we'll be working with
  struct lockt_frame *frame = NULL;

  if (node != NULL) {
    // Frame already exists
    frame = container_of (*node, struct lockt_frame, node);
  } else {
    // Frame doesn't exist - create it
    frame = lockt_new_frame_unsafe (t, lock, e);
    if (frame == NULL) {
      return NULL;
    }
  }

  frame_ref (frame);

  return frame;
}

static err_t
lockt_lock_once (
    struct lockt        *t,
    const struct lt_lock lock,
    const enum lock_mode mode,
    struct txn          *tx,
    error               *e
)
{
  /**
  // Fast path: if this transaction already holds this lock, skip.
  if (tx && txn_haslock (tx, lock))
    {
      return SUCCESS;
    }
  */
  latch_lock (&t->l);

  // Get the lock frame or create it if it doesn't exist
  struct lockt_frame *frame = get_or_create_frame_unsafe (t, lock, e);
  if (frame == NULL) {
    latch_unlock (&t->l);
    return error_trace (e);
  }

  // Add this lock to the list of transaction locks
  if (tx) {
    if (txn_newlock (tx, lock, mode, e) != SUCCESS) {
      frame_unref_unsafe (t, frame);
      latch_unlock (&t->l);
      return error_trace (e);
    }
  }

  latch_unlock (&t->l);

  // Between these two calls, a latch can be acquired on a different
  // thread, which means we could have just created a new frame, then
  // immediately have to wait. But that's ok, the frame creation was
  // just so we can find it and representative of a read

  // This is the main wait logic
  if (gr_lock (&frame->lock, mode, e) != SUCCESS) {
    // This race is hard to test - between locking, and unreffing
    // but seems fine
    latch_lock (&t->l);
    frame_unref_unsafe (t, frame);
    latch_unlock (&t->l);
    return error_trace (e);
  }

  return SUCCESS;
}

static void
lockt_unlock_once (struct lockt *t, const struct lt_lock lock, const enum lock_mode mode)
{
  latch_lock (&t->l);
  {
    // Search for the node
    struct lockt_frame key;
    lockt_frame_init_key (&key, lock);
    struct hnode **found = htable_lookup (t->table, &key.node, lockt_frame_eq);
    ASSERT (found);
    struct lockt_frame *frame = container_of (*found, struct lockt_frame, node);

    // Unlock it
    gr_unlock (&frame->lock, mode);

    // Unreference it
    frame_unref_unsafe (t, frame);
  }
  latch_unlock (&t->l);
}

err_t
lockt_lock (
    struct lockt        *t,
    const struct lt_lock lock,
    const enum lock_mode mode,
    struct txn          *tx,
    error               *e
)
{
  // Fetch and lock the parent lock first if there is one
  struct lt_lock parent;
  bool           has_parent = get_parent (&parent, lock);

  // First, lock the parent (top - down)
  if (has_parent) {
    const enum lock_mode pmode = get_parent_mode (mode);
    if (lockt_lock (t, parent, pmode, tx, e) < 0) {
      return error_trace (e);
    }
  }

  // Then, lock the requested lock
  if (lockt_lock_once (t, lock, mode, tx, e) < 0) {
    // If this lock failed, unlock the parent is all we need to do
    const enum lock_mode pmode = get_parent_mode (mode);
    lockt_unlock (t, parent, pmode);

    return error_trace (e);
  }

  return SUCCESS;
}

void
lockt_unlock (struct lockt *t, const struct lt_lock lock, const enum lock_mode mode)
{
  // Unlock the child first (bottom-up).
  lockt_unlock_once (t, lock, mode);

  // Then unlock the parent.
  struct lt_lock parent;
  if (get_parent (&parent, lock)) {
    const enum lock_mode pmode = get_parent_mode (mode);
    lockt_unlock (t, parent, pmode);
  }
}

static void
unlock_single_lock (const struct lt_lock lock, const enum lock_mode mode, void *ctx)
{
  struct lockt      *t = ctx;

  struct lockt_frame key;
  lockt_frame_init_key (&key, lock);

  struct hnode **found = htable_lookup (t->table, &key.node, lockt_frame_eq);
  ASSERT (found);

  struct lockt_frame *frame = container_of (*found, struct lockt_frame, node);

  gr_unlock (&frame->lock, mode);
  frame_unref_unsafe (t, frame);
}

void
lockt_unlock_tx (struct lockt *t, struct txn *tx)
{
  ASSERT (t);
  ASSERT (tx);

  latch_lock (&t->l);

  txn_foreach_lock (tx, unlock_single_lock, t);

  latch_unlock (&t->l);
  txn_close (tx);
}

static void
i_log_lockt_entry (struct hnode *node, void *ctx)
{
  int                *log_level = ctx;
  struct lockt_frame *frame     = container_of (node, struct lockt_frame, node);
  switch (frame->key.type) {
    case LOCK_DB: {
      i_log (*log_level, "LOCK_DB\n");
    }
  }
}

void
i_log_lockt (int log_level, struct lockt *t)
{
  latch_lock (&t->l);
  htable_foreach (t->table, i_log_lockt_entry, &log_level);
  latch_unlock (&t->l);
}

bool
lt_lock_equal (const struct lt_lock left, const struct lt_lock right)
{
  if (left.type != right.type) {
    return false;
  }

  switch (left.type) {
    case LOCK_DB: {
      return true;
    }
  }
  UNREACHABLE (); // LCOV_EXCL_LINE
}
