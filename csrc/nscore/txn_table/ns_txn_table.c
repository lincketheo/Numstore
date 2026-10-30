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

#include "nscore/txn_table/ns_txn_table.h"

#include "core/ns_csx_assert.h"
#include "core/ns_dbl_buffer.h"
#include "core/ns_testing.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "nscore/lock_table/ns_lock_table.h"

#include <stddef.h>

/******************************************************************************
 * SECTION: Transaction
 ******************************************************************************/

void
txn_init (struct txn *dest, const txid tid, const struct txn_data data, struct i_mem mem)
{
  dest->data  = data;
  dest->tid   = tid;
  dest->locks = NULL;
  hnode_init (&dest->node, tid);
  latch_init (&dest->l);
  slab_alloc_init (&dest->lock_alloc, mem, sizeof (struct txn_lock), 512);
}

void
txn_key_init (struct txn *dest, const txid tid)
{
  dest->tid = tid;
  hnode_init (&dest->node, tid);
  latch_init (&dest->l);
}

void
txn_update_data (struct txn *t, const struct txn_data data)
{
  latch_lock (&t->l);
  t->data = data;
  latch_unlock (&t->l);
}

void
txn_update (struct txn *t, enum tx_state state, const lsn last, const lsn undo_next)
{
  (void)state; // TODO - I think this needs to be used - fix it
  latch_lock (&t->l);
  t->data = (struct txn_data){
      .state         = TX_CANDIDATE_FOR_UNDO,
      .last_lsn      = last,
      .undo_next_lsn = undo_next,
  };
  latch_unlock (&t->l);
}

void
txn_update_state (struct txn *t, const enum tx_state new_state)
{
  latch_lock (&t->l);
  t->data.state = new_state;
  latch_unlock (&t->l);
}

bool
txn_data_equal_unsafe (const struct txn_data *left, const struct txn_data *right)
{
  bool equal = true;

  equal      = ((equal && left->last_lsn == right->last_lsn) != 0);
  equal      = ((equal && left->undo_next_lsn == right->undo_next_lsn) != 0);
  equal      = ((equal && left->state == right->state) != 0);

  return equal;
}

static bool
txn_haslock_unsafe (const struct txn *t, const struct lt_lock lock)
{
  bool                   ret  = false;

  const struct txn_lock *curr = t->locks;
  while (curr != NULL) {
    if (lt_lock_equal (curr->lock, lock)) {
      ret = true;
      goto theend;
    }
    curr = curr->next;
  }

theend:
  return ret;
}

err_t
txn_newlock (struct txn *t, const struct lt_lock lock, const enum lock_mode mode, error *e)
{
  latch_lock (&t->l);

  if (txn_haslock_unsafe (t, lock)) {
    latch_unlock (&t->l);
    return SUCCESS;
  }

  struct txn_lock *next = slab_alloc_alloc (&t->lock_alloc, e);
  if (next == NULL) {
    latch_unlock (&t->l);
    return error_trace (e);
  }

  next->lock = lock;
  next->mode = mode;

  next->next = t->locks;
  t->locks   = next;
  latch_unlock (&t->l);

  return SUCCESS;
}

bool
txn_haslock (struct txn *t, const struct lt_lock lock)
{
  latch_lock (&t->l);

  bool                   ret  = false;

  const struct txn_lock *curr = t->locks;
  while (curr != NULL) {
    if (lt_lock_equal (curr->lock, lock)) {
      ret = true;
      goto theend;
    }
    curr = curr->next;
  }

theend:
  latch_unlock (&t->l);
  return ret;
}

void
txn_close (struct txn *t)
{
  latch_lock (&t->l);

  struct txn_lock *curr = t->locks;
  while (curr != NULL) {
    struct txn_lock *next = curr->next;
    slab_alloc_free (&t->lock_alloc, curr);
    curr = next;
  }

  slab_alloc_destroy (&t->lock_alloc);

  latch_unlock (&t->l);
}

void
txn_foreach_lock (struct txn *t, const lock_func func, void *ctx)
{
  latch_lock (&t->l);

  const struct txn_lock *curr = t->locks;
  while (curr != NULL) {
    func (curr->lock, curr->mode, ctx);
    curr = curr->next;
  }

  latch_unlock (&t->l);
}

#ifndef NDEBUG

static void *
txn_newlock_test (void *_tx)
{
  (void)_tx; // Unused
#  define MAYBE_ADD_LOCK(type, r)                                            \
    lock = r;                                                                \
    if (txn_newlock (tx, lock, LM_X, &e)) {                                  \
      goto failed;                                                           \
    }                                                                        \
    if (!txn_haslock (tx, lock)) {                                           \
      error_causef (&e, ERR_INVALID_ARGUMENT, "Transaction must have lock"); \
      goto failed;                                                           \
    }

  return NULL;
}

TEST (txn_basic)
{
  TEST_CASE ("txn_newlock single threaded")
  {
    struct txn tx;
    txn_init (
        &tx,
        10,
        (struct txn_data){
            .state         = TX_RUNNING,
            .last_lsn      = 10,
            .undo_next_lsn = 5,
        },
        ctx->mem
    );

    for (u32 i = 0; i < 1000; ++i) {
      txn_newlock_test (&tx);
    }

    txn_close (&tx);
  }

  TEST_CASE ("txn_newlock multi threaded")
  {
    struct txn tx;
    txn_init (
        &tx,
        10,
        (struct txn_data){
            .state         = TX_RUNNING,
            .last_lsn      = 10,
            .undo_next_lsn = 5,
        },
        ctx->mem
    );

    i_thread threads[100];
    _Static_assert (MAX_THREADS >= 100, "this test needs 100 live thread slots");

    u32 started = 0;
    for (u32 i = 0; i < 100; ++i) {
      if (ctx->os.table
              ->thread_create (ctx->os.self, &threads[i], txn_newlock_test, &tx, &ctx->e)) {
        break;
      }
      started++;
    }

    for (u32 i = 0; i < started; ++i) {
      ctx->os.table->thread_join (ctx->os.self, threads[i].self);
    }

    test_assert_int_equal ((int)started, 100);

    txn_close (&tx);
  }
}

TEST (txn_haslock)
{
  TEST_CASE ("An empty transaction holds nothing")
  {
    struct txn tx;
    txn_init (
        &tx,
        10,
        (struct txn_data){
            .state         = TX_RUNNING,
            .last_lsn      = 10,
            .undo_next_lsn = 5,
        },
        ctx->mem
    );

    // Walks the whole (empty) list and falls out of the loop
    test_assert (!txn_haslock (&tx, lock_db ()));

    txn_close (&tx);
  }

  TEST_CASE ("A taken lock is reported held")
  {
    struct txn tx;
    txn_init (
        &tx,
        10,
        (struct txn_data){
            .state         = TX_RUNNING,
            .last_lsn      = 10,
            .undo_next_lsn = 5,
        },
        ctx->mem
    );

    test_assert_int_equal (txn_newlock (&tx, lock_db (), LM_X, &ctx->e), SUCCESS);
    test_assert (txn_haslock (&tx, lock_db ()));

    // Re-taking it is a no-op, and it is still held afterwards
    test_assert_int_equal (txn_newlock (&tx, lock_db (), LM_X, &ctx->e), SUCCESS);
    test_assert (txn_haslock (&tx, lock_db ()));

    txn_close (&tx);
  }

  TEST_CASE ("Closing gives the locks back")
  {
    struct txn tx;
    txn_init (
        &tx,
        10,
        (struct txn_data){
            .state         = TX_RUNNING,
            .last_lsn      = 10,
            .undo_next_lsn = 5,
        },
        ctx->mem
    );

    test_assert_int_equal (txn_newlock (&tx, lock_db (), LM_X, &ctx->e), SUCCESS);
    test_assert (txn_haslock (&tx, lock_db ()));
    txn_close (&tx);

    // A reinitialised txn starts over, it does not inherit the old list
    txn_init (
        &tx,
        11,
        (struct txn_data){
            .state         = TX_RUNNING,
            .last_lsn      = 11,
            .undo_next_lsn = 6,
        },
        ctx->mem
    );
    test_assert (!txn_haslock (&tx, lock_db ()));

    txn_close (&tx);
  }
}
#endif

/******************************************************************************
 * SECTION: Transaction Table
 ******************************************************************************/

DEFINE_DBG_ASSERT (struct txn_table, txn_table, t, { ASSERT (t); })

#define TXN_SERIAL_UNIT (sizeof (txid) + sizeof (lsn) + sizeof (lsn))

struct txn_table
{
  struct i_mem   mem;
  latch          l;
  struct htable *t;

  bool           isfrozen;
};

struct txn_table *
txnt_open (struct i_mem mem, error *e)
{
  struct txn_table *dest = mem.table->malloc (mem.self, 1, sizeof *dest, e);
  if (dest == NULL) {
    goto failed;
  }

  dest->mem = mem;
  dest->t   = htable_create (512, mem, e);
  if (dest->t == NULL) {
    goto dest_failed;
  }

  latch_init (&dest->l);

  return dest;

dest_failed:
  mem.table->free (mem.self, dest);
failed:
  return NULL;
}

void
txnt_close (struct txn_table *t)
{
  DBG_ASSERT (txn_table, t);
  struct i_mem mem = t->mem;
  latch_lock (&t->l);
  htable_free (t->t);
  latch_unlock (&t->l);
  mem.table->free (mem.self, t);
}

#ifndef NDEBUG
TEST (txnt_open)
{
  TEST_CASE ("basic")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    txnt_close (t);
  }

  TEST_CASE ("open multiple")
  {
    for (int i = 0; i < 4; ++i) {
      struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
      txnt_close (t);
    }
  }
}
#endif

struct txn_merge_ctx
{
  struct txn_table  *dest;
  error             *e;
  struct dbl_buffer *txn_dest;
  struct slab_alloc *alloc;
  struct i_mem       mem;
};

static void
merge_txn (struct txn *tx, void *vctx)
{
  const struct txn_merge_ctx *ctx = vctx;
  ASSERT (ctx->txn_dest == NULL || ctx->txn_dest->size == sizeof (struct txn));

  // Fail fast on an error
  if (ctx->e->cause_code) {
    return;
  }

  latch_lock (&tx->l);

  // Skip duplicate transactions
  if (txn_exists (ctx->dest, tx->tid)) {
    goto theend;
  }

  // Handle in case we copy transaction over
  struct txn *target_txn = tx;

  // If provided, allocate a new transaction to copy over
  if (ctx->txn_dest && ctx->alloc) {
    target_txn = slab_alloc_alloc (ctx->alloc, ctx->e);
    if (target_txn == NULL) {
      goto theend;
    }
    if (dblb_append (ctx->txn_dest, &target_txn, 1, ctx->e)) {
      goto theend;
    }
    txn_init (target_txn, tx->tid, tx->data, ctx->mem);
  }

  // Insert into the table
  txnt_insert_txn (ctx->dest, target_txn);

theend:
  latch_unlock (&tx->l);
}

err_t
txnt_merge_into (
    struct txn_table  *dest,
    struct txn_table  *src,
    struct dbl_buffer *txn_dest,
    struct slab_alloc *alloc,
    struct i_mem       mem,
    error             *e
)
{
  struct txn_merge_ctx ctx = {
      .dest     = dest,
      .e        = e,
      .txn_dest = txn_dest,
      .alloc    = alloc,
      .mem      = mem,
  };

  latch_lock (&src->l);
  txnt_foreach (src, merge_txn, &ctx);
  latch_unlock (&src->l);

  return ctx.e->cause_code;
}

#ifndef NDEBUG
TEST (txnt_merge_into)
{
  TEST_CASE ("Empty to Empty")
  {
    struct txn_table *src    = txnt_open (ctx->mem, &ctx->e);
    struct txn_table *dest   = txnt_open (ctx->mem, &ctx->e);
    const err_t       result = txnt_merge_into (dest, src, NULL, NULL, ctx->mem, &ctx->e);
    test_assert (result == SUCCESS);

    txnt_close (dest);
    txnt_close (src);
  }

  TEST_CASE ("Data")
  {
    struct txn_table *dest = txnt_open (ctx->mem, &ctx->e);
    struct txn_table *src  = txnt_open (ctx->mem, &ctx->e);
    // Add to dest
    struct txn        dest_txns[5];
    for (int i = 0; i < 5; i++) {
      txn_init (
          &dest_txns[i],
          i + 1,
          (struct txn_data){
              .last_lsn      = (i + 1) * 10,
              .undo_next_lsn = (i + 1) * 10 - 1,
              .state         = TX_RUNNING,
          },
          ctx->mem
      );
      txnt_insert_txn (dest, &dest_txns[i]);
    }

    // Add to src (different tids)
    struct txn src_txns[5];
    for (int i = 0; i < 5; i++) {
      txn_init (
          &src_txns[i],
          i + 6,
          (struct txn_data){
              .last_lsn      = (i + 6) * 10,
              .undo_next_lsn = (i + 6) * 10 - 1,
              .state         = TX_CANDIDATE_FOR_UNDO,
          },
          ctx->mem
      );
      txnt_insert_txn (src, &src_txns[i]);
    }

    const err_t result = txnt_merge_into (dest, src, NULL, NULL, ctx->mem, &ctx->e);
    test_assert (result == SUCCESS);

    // Verify all exist in dest
    for (txid tid = 1; tid <= 10; tid++) {
      test_assert (txn_exists (dest, tid));
    }

    txnt_close (dest);
    txnt_close (src);
  }

  TEST_CASE ("no duplicate insert")
  {
    struct txn_table *dest = txnt_open (ctx->mem, &ctx->e);
    struct txn_table *src  = txnt_open (ctx->mem, &ctx->e);
    // Add same tid to both with different values
    struct txn        dest_tx;
    txn_init (
        &dest_tx,
        42,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );
    txnt_insert_txn (dest, &dest_tx);

    struct txn src_tx;
    txn_init (
        &src_tx,
        42,
        (struct txn_data){
            .last_lsn      = 200,
            .undo_next_lsn = 190,
            .state         = TX_COMMITTED,
        },
        ctx->mem
    );
    txnt_insert_txn (src, &src_tx);

    txnt_merge_into (dest, src, NULL, NULL, ctx->mem, &ctx->e);

    // Dest should keep its original value
    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, dest, 42);
    test_assert (found);
    test_assert_int_equal (retrieved->data.last_lsn, 100);
    test_assert_int_equal (retrieved->data.state, TX_RUNNING);

    txnt_close (dest);
    txnt_close (src);
  }
}
#endif

static void
find_max_undo (struct txn *tx, void *vctx)
{
  slsn *max = vctx;

  latch_lock (&tx->l);

  if ((tx->data.state == TX_CANDIDATE_FOR_UNDO) && ((slsn)tx->data.undo_next_lsn > *max)) {
    *max = tx->data.undo_next_lsn;
  }

  latch_unlock (&tx->l);
}

slsn
txnt_max_u_undo_lsn (struct txn_table *t)
{
  slsn max = -1;

  latch_lock (&t->l);
  txnt_foreach (t, find_max_undo, &max);
  latch_unlock (&t->l);

  return max;
}

#ifndef NDEBUG
TEST (txnt_max_u_undo_lsn)
{
  TEST_CASE ("empty")
  {
    struct txn_table *t   = txnt_open (ctx->mem, &ctx->e);
    slsn              max = txnt_max_u_undo_lsn (t);
    test_assert (max == -1);

    txnt_close (t);
  }

  TEST_CASE ("candidates")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx1, tx2, tx3, tx4;

    // Running - should be ignored
    txn_init (
        &tx1,
        1,
        (struct txn_data){.last_lsn = 100, .undo_next_lsn = 100, .state = TX_RUNNING},
        ctx->mem
    );

    // Candidates
    txn_init (
        &tx2,
        2,
        (struct txn_data){.last_lsn = 50, .undo_next_lsn = 40, .state = TX_CANDIDATE_FOR_UNDO},
        ctx->mem
    );
    txn_init (
        &tx3,
        3,
        (struct txn_data){.last_lsn = 80, .undo_next_lsn = 75, .state = TX_CANDIDATE_FOR_UNDO},
        ctx->mem
    );
    txn_init (
        &tx4,
        4,
        (struct txn_data){.last_lsn = 60, .undo_next_lsn = 55, .state = TX_CANDIDATE_FOR_UNDO},
        ctx->mem
    );

    txnt_insert_txn (t, &tx1);
    txnt_insert_txn (t, &tx2);
    txnt_insert_txn (t, &tx3);
    txnt_insert_txn (t, &tx4);

    slsn max = txnt_max_u_undo_lsn (t);
    test_assert (max == 75);

    txnt_close (t);
  }

  TEST_CASE ("only running txns")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx1, tx2, tx3;
    txn_init (
        &tx1,
        1,
        (struct txn_data){.last_lsn = 100, .undo_next_lsn = 90, .state = TX_RUNNING},
        ctx->mem
    );
    txn_init (
        &tx2,
        2,
        (struct txn_data){.last_lsn = 200, .undo_next_lsn = 190, .state = TX_RUNNING},
        ctx->mem
    );
    txn_init (
        &tx3,
        3,
        (struct txn_data){.last_lsn = 300, .undo_next_lsn = 290, .state = TX_COMMITTED},
        ctx->mem
    );

    txnt_insert_txn (t, &tx1);
    txnt_insert_txn (t, &tx2);
    txnt_insert_txn (t, &tx3);

    slsn max = txnt_max_u_undo_lsn (t);
    test_assert (max == -1);

    txnt_close (t);
  }
}
#endif

static void
find_min_lsn (struct txn *tx, void *vctx)
{
  slsn *min = vctx;

  latch_lock (&tx->l);

  if (*min == -1 || (slsn)tx->data.min_lsn < *min) {
    *min = tx->data.min_lsn;
  }

  latch_unlock (&tx->l);
}

slsn
txnt_min_lsn (struct txn_table *t)
{
  slsn min = -1; // Actually a big number b/c unsigned

  latch_lock (&t->l);
  txnt_foreach (t, find_min_lsn, &min);
  latch_unlock (&t->l);

  return min;
}

#ifndef NDEBUG
TEST (txnt_min_lsn)
{
  TEST_CASE ("empty")
  {
    struct txn_table *t   = txnt_open (ctx->mem, &ctx->e);
    const slsn        min = txnt_min_lsn (t);
    test_assert (min == -1);

    txnt_close (t);
  }

  TEST_CASE ("candidates")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx1, tx2, tx3, tx4;

    txn_init (&tx1, 1, (struct txn_data){.min_lsn = 100}, ctx->mem);
    txn_init (&tx2, 2, (struct txn_data){.min_lsn = 40}, ctx->mem);
    txn_init (&tx3, 3, (struct txn_data){.min_lsn = 75}, ctx->mem);
    txn_init (&tx4, 4, (struct txn_data){.min_lsn = 55}, ctx->mem);

    txnt_insert_txn (t, &tx1);
    txnt_insert_txn (t, &tx2);
    txnt_insert_txn (t, &tx3);
    txnt_insert_txn (t, &tx4);

    const slsn min = txnt_min_lsn (t);
    test_assert (min == 40);

    txnt_close (t);
  }
}
#endif

struct txn_foreach_ctx
{
  void (*action) (struct txn *, void *ctx);
  void *ctx;
};

static void
hnode_foreach (struct hnode *node, void *ctx)
{
  const struct txn_foreach_ctx *_ctx = ctx;
  _ctx->action (container_of (node, struct txn, node), _ctx->ctx);
}

void
txnt_foreach (const struct txn_table *t, void (*action) (struct txn *, void *ctx), void *ctx)
{
  struct txn_foreach_ctx _ctx = {
      .action = action,
      .ctx    = ctx,
  };
  htable_foreach (t->t, hnode_foreach, &_ctx);
}

u32
txnt_get_size (const struct txn_table *dest)
{
  return htable_size (dest->t);
}

static bool
txn_equals_for_exists (const struct hnode *left, const struct hnode *right)
{
  // Might have passed the exact same ref as exists in the htable
  if (left == right) {
    return true;
  }

  // Otherwise, passed a key with just relevant information

  struct txn *_left  = container_of (left, struct txn, node);
  struct txn *_right = container_of (right, struct txn, node);

  latch_lock (&_left->l);
  latch_lock (&_right->l);

  bool ret = _left->tid == _right->tid;

  latch_unlock (&_right->l);
  latch_unlock (&_left->l);

  return ret;
}

bool
txn_exists (const struct txn_table *t, const txid tid)
{
  struct txn tx;
  txn_key_init (&tx, tid);

  struct hnode **ret = htable_lookup (t->t, &tx.node, txn_equals_for_exists);

  return ret != NULL;
}

#ifndef NDEBUG
TEST (txnt_exists)
{
  TEST_CASE ("txnt_exists")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    test_assert (!txn_exists (t, 1100));

    struct txn tx;
    txn_init (
        &tx,
        1100,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);
    test_assert (txn_exists (t, 1100));

    txnt_close (t);
  }
}
#endif

void
txnt_insert_txn (struct txn_table *t, struct txn *tx)
{
  DBG_ASSERT (txn_table, t);
  ASSERT (!txn_exists (t, tx->tid));

  latch_lock (&t->l);
  htable_insert (t->t, &tx->node);
  latch_unlock (&t->l);
}

void
txnt_insert_txn_if_not_exists (struct txn_table *t, struct txn *tx)
{
  DBG_ASSERT (txn_table, t);

  latch_lock (&t->l);

  if (txn_exists (t, tx->tid)) {
    goto theend;
  }

  htable_insert (t->t, &tx->node);

theend:
  latch_unlock (&t->l);
}

#ifndef NDEBUG
TEST (txnt_insert)
{
  TEST_CASE ("new")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        900,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn_if_not_exists (t, &tx);

    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 900);
    test_assert (found);
    test_assert (retrieved->tid == 900);

    txnt_close (t);
  }

  TEST_CASE ("if not exists but exists")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx1;
    txn_init (
        &tx1,
        1000,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx1);

    struct txn tx2;
    txn_init (
        &tx2,
        1000,
        (struct txn_data){
            .last_lsn      = 200,
            .undo_next_lsn = 180,
            .state         = TX_COMMITTED,
        },
        ctx->mem
    );

    txnt_insert_txn_if_not_exists (t, &tx2);

    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 1000);
    test_assert (found);
    test_assert (retrieved->data.last_lsn == 100);
    test_assert (retrieved->data.state == TX_RUNNING);

    txnt_close (t);
  }

  TEST_CASE ("different states")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx1, tx2, tx3;

    txn_init (
        &tx1,
        1,
        (struct txn_data){.last_lsn = 10, .undo_next_lsn = 9, .state = TX_RUNNING},
        ctx->mem
    );
    txn_init (
        &tx2,
        2,
        (struct txn_data){.last_lsn = 20, .undo_next_lsn = 19, .state = TX_CANDIDATE_FOR_UNDO},
        ctx->mem
    );
    txn_init (
        &tx3,
        3,
        (struct txn_data){.last_lsn = 30, .undo_next_lsn = 29, .state = TX_COMMITTED},
        ctx->mem
    );

    txnt_insert_txn (t, &tx1);
    txnt_insert_txn (t, &tx2);
    txnt_insert_txn (t, &tx3);

    struct txn *retrieved;

    test_assert (txnt_get (&retrieved, t, 1));
    test_assert (retrieved->data.state == TX_RUNNING);

    test_assert (txnt_get (&retrieved, t, 2));
    test_assert (retrieved->data.state == TX_CANDIDATE_FOR_UNDO);

    test_assert (txnt_get (&retrieved, t, 3));
    test_assert (retrieved->data.state == TX_COMMITTED);

    txnt_close (t);
  }
}
#endif

bool
txnt_get (struct txn **dest, struct txn_table *t, const txid tid)
{
  DBG_ASSERT (txn_table, t);

  struct txn key;
  txn_key_init (&key, tid);

  latch_lock (&t->l);

  struct hnode **node = htable_lookup (t->t, &key.node, txn_equals_for_exists);
  if (node) {
    *dest = container_of (*node, struct txn, node);
  }

  latch_unlock (&t->l);

  return node != NULL;
}

void
txnt_get_expect (struct txn **dest, struct txn_table *t, const txid tid)
{
  DBG_ASSERT (txn_table, t);

  struct txn key;
  txn_key_init (&key, tid);

  latch_lock (&t->l);

  struct hnode **node = htable_lookup (t->t, &key.node, txn_equals_for_exists);
  ASSERT (node);
  *dest = container_of (*node, struct txn, node);

  latch_unlock (&t->l);
}

#ifndef NDEBUG
TEST (txnt_get)
{
  TEST_CASE ("nonexistent returns false")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn       *retrieved;
    bool              found = txnt_get (&retrieved, t, 9999);
    test_assert (!found);

    txnt_close (t);
  }

  TEST_CASE ("and get tx running")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        100,
        (struct txn_data){
            .last_lsn      = 50,
            .undo_next_lsn = 40,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 100);
    test_assert (found);
    test_assert (txn_data_equal_unsafe (&retrieved->data, &tx.data));

    txnt_close (t);
  }

  TEST_CASE ("and get tx candidate for undo")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        200,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_CANDIDATE_FOR_UNDO,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 200);
    test_assert (found);
    test_assert (retrieved->tid == 200);
    test_assert (retrieved->data.last_lsn == 100);
    test_assert (retrieved->data.undo_next_lsn == 90);
    test_assert (retrieved->data.state == TX_CANDIDATE_FOR_UNDO);

    txnt_close (t);
  }

  TEST_CASE ("update last lsn")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        600,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    // Fetch, update, verify
    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 600);
    test_assert (found);

    struct txn_data new_data = retrieved->data;
    new_data.last_lsn        = 200;
    txn_update_data (retrieved, new_data);

    // Verify update
    found = txnt_get (&retrieved, t, 600);
    test_assert (found);
    test_assert (retrieved->data.last_lsn == 200);

    txnt_close (t);
  }

  TEST_CASE ("txnt state transitions all types")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        2000,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 99,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    struct txn  key;
    struct txn *retrieved = &key;

    test_assert (txnt_get (&retrieved, t, 2000));
    test_assert (retrieved->data.state == TX_RUNNING);

    // Transition to CANDIDATE_FOR_UNDO
    struct txn_data new_data = retrieved->data;
    new_data.state           = TX_CANDIDATE_FOR_UNDO;
    txn_update_data (retrieved, new_data);

    test_assert (txnt_get (&retrieved, t, 2000));
    test_assert (retrieved->data.state == TX_CANDIDATE_FOR_UNDO);

    // Transition to COMMITTED
    new_data       = retrieved->data;
    new_data.state = TX_COMMITTED;
    txn_update_data (retrieved, new_data);

    test_assert (txnt_get (&retrieved, t, 2000));
    test_assert (retrieved->data.state == TX_COMMITTED);

    txnt_close (t);
  }
}
#endif

void
txnt_remove_txn (bool *exists, struct txn_table *t, const struct txn *tx)
{
  DBG_ASSERT (txn_table, t);

  latch_lock (&t->l);

  struct hnode **node = htable_lookup (t->t, &tx->node, txn_equals_for_exists);

  if (node == NULL) {
    *exists = false;
    goto theend;
  }

  *exists = true;

  htable_delete (t->t, node);

theend:
  latch_unlock (&t->l);
}

void
txnt_remove_txn_expect (struct txn_table *t, const struct txn *tx)
{
  DBG_ASSERT (txn_table, t);

  latch_lock (&t->l);

  struct hnode **node = htable_lookup (t->t, &tx->node, txn_equals_for_exists);

  ASSERT (node != NULL);

  htable_delete (t->t, node);

  latch_unlock (&t->l);
}

#ifndef NDEBUG
TEST (txnt_remove)
{
  TEST_CASE ("txnt_remove_existing_txn")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        400,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    bool removed;
    txnt_remove_txn (&removed, t, &tx);
    test_assert (removed);

    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 400);
    test_assert (!found);

    txnt_close (t);
  }

  TEST_CASE ("txnt_remove_nonexistent_txn")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_key_init (&tx, 500);

    bool removed;
    txnt_remove_txn (&removed, t, &tx);
    test_assert (!removed);

    txnt_close (t);
  }

  TEST_CASE ("txnt_double_remove_same_transaction")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        100,
        (struct txn_data){
            .last_lsn      = 50,
            .undo_next_lsn = 49,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    bool removed;
    txnt_remove_txn (&removed, t, &tx);
    test_assert (removed);

    txnt_remove_txn (&removed, t, &tx);
    test_assert (!removed);

    txnt_close (t);
  }

  TEST_CASE ("txnt_operations_after_remove")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        200,
        (struct txn_data){
            .last_lsn      = 50,
            .undo_next_lsn = 49,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    bool removed;
    txnt_remove_txn (&removed, t, &tx);
    test_assert (removed);

    // Get should fail
    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 200);
    test_assert (!found);

    txnt_close (t);
  }
}
#endif

struct txnt_eq_ctx
{
  struct txn_table *other;
  bool              ret;
};

static void
txnt_eq_foreach (struct hnode *node, void *_ctx)
{
  struct txnt_eq_ctx *ctx = _ctx;
  if ((int)ctx->ret == false) {
    return;
  }

  struct txn *tx = container_of (node, struct txn, node);
  struct txn  candidate;

  latch_lock (&tx->l);
  {
    txn_key_init (&candidate, tx->tid);

    struct hnode **other_node = htable_lookup (
        ctx->other->t,
        &candidate.node,
        txn_equals_for_exists
    );

    if (other_node == NULL) {
      ctx->ret = false;
      latch_unlock (&tx->l);
      return;
    }

    struct txn *other_tx = container_of (*other_node, struct txn, node);

    latch_lock (&other_tx->l);
    {
      bool equal = true;

      equal      = ((equal && tx->data.last_lsn == other_tx->data.last_lsn) != 0);
      equal      = ((equal && tx->data.undo_next_lsn == other_tx->data.undo_next_lsn) != 0);

      ctx->ret   = equal;
    }
    latch_unlock (&other_tx->l);
  }
  latch_unlock (&tx->l);
}

bool
txnt_equal_ignore_state (struct txn_table *left, struct txn_table *right)
{
  latch_lock (&left->l);
  latch_lock (&right->l);

  if (htable_size (left->t) != htable_size (right->t)) {
    latch_unlock (&right->l);
    latch_unlock (&left->l);
    return false;
  }

  struct txnt_eq_ctx ctx = {
      .other = right,
      .ret   = true,
  };
  htable_foreach (left->t, txnt_eq_foreach, &ctx);

  latch_unlock (&right->l);
  latch_unlock (&left->l);

  return ctx.ret;
}

#ifndef NDEBUG
TEST (txnt_equal_ignore_state)
{
  TEST_CASE ("txnt_equal_ignore_state_empty_tables")
  {
    struct txn_table *t1 = txnt_open (ctx->mem, &ctx->e);
    struct txn_table *t2 = txnt_open (ctx->mem, &ctx->e);
    test_assert (txnt_equal_ignore_state (t1, t2));

    txnt_close (t1);
    txnt_close (t2);
  }

  TEST_CASE ("txnt_equal_ignore_state_same_content")
  {
    struct txn_table *t1 = txnt_open (ctx->mem, &ctx->e);
    struct txn_table *t2 = txnt_open (ctx->mem, &ctx->e);
    struct txn        t1_txns[5], t2_txns[5];
    for (int i = 0; i < 5; i++) {
      txn_init (
          &t1_txns[i],
          i + 1,
          (struct txn_data){
              .last_lsn      = (i + 1) * 10,
              .undo_next_lsn = (i + 1) * 10 - 1,
              .state         = TX_RUNNING,
          },
          ctx->mem
      );
      txn_init (
          &t2_txns[i],
          i + 1,
          (struct txn_data){
              .last_lsn      = (i + 1) * 10,
              .undo_next_lsn = (i + 1) * 10 - 1,
              .state         = TX_RUNNING,
          },
          ctx->mem
      );
      txnt_insert_txn (t1, &t1_txns[i]);
      txnt_insert_txn (t2, &t2_txns[i]);
    }

    test_assert (txnt_equal_ignore_state (t1, t2));

    txnt_close (t1);
    txnt_close (t2);
  }

  TEST_CASE ("txnt_not_equal_different_content")
  {
    struct txn_table *t1 = txnt_open (ctx->mem, &ctx->e);
    struct txn_table *t2 = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx1, tx2;
    txn_init (
        &tx1,
        1,
        (struct txn_data){.last_lsn = 10, .undo_next_lsn = 9, .state = TX_RUNNING},
        ctx->mem
    );
    txn_init (
        &tx2,
        1,
        (struct txn_data){.last_lsn = 20, .undo_next_lsn = 19, .state = TX_RUNNING},
        ctx->mem
    );

    txnt_insert_txn (t1, &tx1);
    txnt_insert_txn (t2, &tx2);

    test_assert (!txnt_equal_ignore_state (t1, t2));

    txnt_close (t1);
    txnt_close (t2);
  }
}
#endif

void
txnt_crash (struct txn_table *t)
{
  DBG_ASSERT (txn_table, t);
  struct i_mem mem = t->mem;
  htable_free (t->t);
  mem.table->free (mem.self, t);
}

#ifndef NDEBUG
#  include <stdatomic.h>

struct txnt_thread_ctx
{
  struct txn_table *table;
  struct txn       *txn_bank; // Pre-allocated transactions
  _Atomic int       counter;
  txid              start_tid;
  int               count;
  struct i_mem      mem;
};

static void *
txnt_insert_thread (void *arg)
{
  struct txnt_thread_ctx *ctx = arg;

  for (int i = 0; i < ctx->count; i++) {
    txn_init (
        &ctx->txn_bank[i],
        ctx->start_tid + i,
        (struct txn_data){
            .last_lsn      = ctx->start_tid + i,
            .undo_next_lsn = ctx->start_tid + i - 1,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (ctx->table, &ctx->txn_bank[i]);

    atomic_fetch_add (&ctx->counter, 1);
  }

  return NULL;
}

static void *
txnt_reader_thread (void *arg)
{
  struct txnt_thread_ctx *ctx = arg;

  for (int i = 0; i < ctx->count; i++) {
    struct txn *retrieved;
    if (txnt_get (&retrieved, ctx->table, ctx->start_tid + i)) {
      atomic_fetch_add (&ctx->counter, 1);
    }
  }

  return NULL;
}

static void *
txnt_updater_thread (void *arg)
{
  struct txnt_thread_ctx *ctx = arg;

  for (int i = 0; i < ctx->count; i++) {
    struct txn *retrieved;
    if (txnt_get (&retrieved, ctx->table, ctx->start_tid + i)) {
      struct txn_data new_data = retrieved->data;
      new_data.last_lsn        = ctx->start_tid + i + 1000;
      txn_update_data (retrieved, new_data);
      atomic_fetch_add (&ctx->counter, 1);
    }
  }

  return NULL;
}

static void *
txnt_state_transition_thread (void *arg)
{
  struct txnt_thread_ctx *ctx = arg;

  for (int i = 0; i < ctx->count; i++) {
    struct txn *retrieved;
    if (txnt_get (&retrieved, ctx->table, ctx->start_tid + i)) {
      // TX_RUNNING -> TX_CANDIDATE_FOR_UNDO
      struct txn_data new_data = retrieved->data;
      new_data.state           = TX_CANDIDATE_FOR_UNDO;
      txn_update_data (retrieved, new_data);

      atomic_fetch_add (&ctx->counter, 1);
      i_sleep_us (100);

      // -> TX_COMMITTED
      new_data.state = TX_COMMITTED;
      txn_update_data (retrieved, new_data);
    }
  }

  return NULL;
}

TEST (txnt_concurrent)
{
  TEST_CASE ("txnt_concurrent_inserts")
  {
    struct txn_table      *t = txnt_open (ctx->mem, &ctx->e);
    struct txn             txn_bank1[100], txn_bank2[100], txn_bank3[100];

    struct txnt_thread_ctx ctx1 = {
        .table     = t,
        .txn_bank  = txn_bank1,
        .start_tid = 0,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx2 = {
        .table     = t,
        .txn_bank  = txn_bank2,
        .start_tid = 100,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx3 = {
        .table     = t,
        .txn_bank  = txn_bank3,
        .start_tid = 200,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };

    i_thread t1, t2, t3;
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t1, txnt_insert_thread, &ctx1, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t2, txnt_insert_thread, &ctx2, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t3, txnt_insert_thread, &ctx3, &ctx->e),
        SUCCESS
    );

    ctx->os.table->thread_join (ctx->os.self, t1.self);
    ctx->os.table->thread_join (ctx->os.self, t2.self);
    ctx->os.table->thread_join (ctx->os.self, t3.self);

    int total_inserts = ctx1.counter + ctx2.counter + ctx3.counter;
    test_assert_equal (total_inserts, 300);

    for (txid tid = 0; tid < 300; tid++) {
      test_assert (txn_exists (t, tid));
    }

    txnt_close (t);
  }

  TEST_CASE ("txnt_concurrent_readers")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    // Pre-populate
    struct txn        txns[200];
    for (int i = 0; i < 200; i++) {
      txn_init (
          &txns[i],
          i,
          (struct txn_data){
              .last_lsn      = i,
              .undo_next_lsn = i - 1,
              .state         = TX_RUNNING,
          },
          ctx->mem
      );
      txnt_insert_txn (t, &txns[i]);
    }

    struct txnt_thread_ctx ctx1 = {
        .table     = t,
        .start_tid = 0,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx2 = {
        .table     = t,
        .start_tid = 50,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx3 = {
        .table     = t,
        .start_tid = 100,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };

    i_thread t1, t2, t3;
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t1, txnt_reader_thread, &ctx1, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t2, txnt_reader_thread, &ctx2, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t3, txnt_reader_thread, &ctx3, &ctx->e),
        SUCCESS
    );

    ctx->os.table->thread_join (ctx->os.self, t1.self);
    ctx->os.table->thread_join (ctx->os.self, t2.self);
    ctx->os.table->thread_join (ctx->os.self, t3.self);

    int total_reads = ctx1.counter + ctx2.counter + ctx3.counter;
    test_assert_equal (total_reads, 300);

    txnt_close (t);
  }

  TEST_CASE ("txnt_update_undo_next")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        700,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 80,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 700);
    test_assert (found);

    struct txn_data new_data = retrieved->data;
    new_data.undo_next_lsn   = 150;
    txn_update_data (retrieved, new_data);

    found = txnt_get (&retrieved, t, 700);
    test_assert (found);
    test_assert (retrieved->data.undo_next_lsn == 150);

    txnt_close (t);
  }

  TEST_CASE ("txnt_update_state")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    struct txn        tx;
    txn_init (
        &tx,
        800,
        (struct txn_data){
            .last_lsn      = 100,
            .undo_next_lsn = 90,
            .state         = TX_RUNNING,
        },
        ctx->mem
    );

    txnt_insert_txn (t, &tx);

    struct txn *retrieved;
    bool        found = txnt_get (&retrieved, t, 800);
    test_assert (found);

    struct txn_data new_data = retrieved->data;
    new_data.state           = TX_COMMITTED;
    txn_update_data (retrieved, new_data);

    found = txnt_get (&retrieved, t, 800);
    test_assert (found);
    test_assert (retrieved->data.state == TX_COMMITTED);

    txnt_close (t);
  }

  TEST_CASE ("txnt_concurrent_updates")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    // Pre-populate
    struct txn        txns[300];
    for (int i = 0; i < 300; i++) {
      txn_init (
          &txns[i],
          i,
          (struct txn_data){
              .last_lsn      = i,
              .undo_next_lsn = i - 1,
              .state         = TX_RUNNING,
          },
          ctx->mem
      );
      txnt_insert_txn (t, &txns[i]);
    }

    struct txnt_thread_ctx ctx1 = {
        .table     = t,
        .start_tid = 0,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx2 = {
        .table     = t,
        .start_tid = 100,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx3 = {
        .table     = t,
        .start_tid = 200,
        .count     = 100,
        .counter   = 0,
        .mem       = ctx->mem,
    };

    i_thread t1, t2, t3;
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t1, txnt_updater_thread, &ctx1, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t2, txnt_updater_thread, &ctx2, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table->thread_create (ctx->os.self, &t3, txnt_updater_thread, &ctx3, &ctx->e),
        SUCCESS
    );

    ctx->os.table->thread_join (ctx->os.self, t1.self);
    ctx->os.table->thread_join (ctx->os.self, t2.self);
    ctx->os.table->thread_join (ctx->os.self, t3.self);

    int total_updates = ctx1.counter + ctx2.counter + ctx3.counter;
    test_assert_equal (total_updates, 300);

    // Verify updates
    for (txid tid = 0; tid < 300; tid++) {
      struct txn *retrieved;
      test_assert (txnt_get (&retrieved, t, tid));
      test_assert_equal (retrieved->data.last_lsn, tid + 1000);
    }

    txnt_close (t);
  }

  TEST_CASE ("txnt_concurrent_state_transitions")
  {
    struct txn_table *t = txnt_open (ctx->mem, &ctx->e);
    // Pre-populate with running transactions
    struct txn        txns[150];
    for (int i = 0; i < 150; i++) {
      txn_init (
          &txns[i],
          i,
          (struct txn_data){
              .last_lsn      = i,
              .undo_next_lsn = i - 1,
              .state         = TX_RUNNING,
          },
          ctx->mem
      );
      txnt_insert_txn (t, &txns[i]);
    }

    struct txnt_thread_ctx ctx1 = {
        .table     = t,
        .start_tid = 0,
        .count     = 50,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx2 = {
        .table     = t,
        .start_tid = 50,
        .count     = 50,
        .counter   = 0,
        .mem       = ctx->mem,
    };
    struct txnt_thread_ctx ctx3 = {
        .table     = t,
        .start_tid = 100,
        .count     = 50,
        .counter   = 0,
        .mem       = ctx->mem,
    };

    i_thread t1, t2, t3;
    test_assert_equal (
        ctx->os.table
            ->thread_create (ctx->os.self, &t1, txnt_state_transition_thread, &ctx1, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table
            ->thread_create (ctx->os.self, &t2, txnt_state_transition_thread, &ctx2, &ctx->e),
        SUCCESS
    );
    test_assert_equal (
        ctx->os.table
            ->thread_create (ctx->os.self, &t3, txnt_state_transition_thread, &ctx3, &ctx->e),
        SUCCESS
    );

    ctx->os.table->thread_join (ctx->os.self, t1.self);
    ctx->os.table->thread_join (ctx->os.self, t2.self);
    ctx->os.table->thread_join (ctx->os.self, t3.self);

    int total_transitions = ctx1.counter + ctx2.counter + ctx3.counter;
    test_assert_equal (total_transitions, 150);

    // Verify all are committed
    for (txid tid = 0; tid < 150; tid++) {
      struct txn *retrieved;
      test_assert (txnt_get (&retrieved, t, tid));
      test_assert_equal (retrieved->data.state, TX_COMMITTED);
    }

    txnt_close (t);
  }
}

#endif
