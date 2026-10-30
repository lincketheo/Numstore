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

#include "core/ns_gr_lock.h"

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_latch.h"
#include "core/ns_testing.h"
#include "core/os/ns_os.h"

#ifndef NDEBUG
#  include "core/ns_logging.h"
#endif

#include <stdint.h>
#include <string.h>

/******************************************************************************
 * SECTION: GR Lock
 ******************************************************************************/

// clang-format off
static const bool compatible[LM_COUNT][LM_COUNT] = {
  //         IS      IX      S       SIX     X
  [LM_IS]  = { true,  true,  true,  true,  false },
  [LM_IX]  = { true,  true,  false, false, false },
  [LM_S]   = { true,  false, true,  false, false },
  [LM_SIX] = { true,  false, false, false, false },
  [LM_X]   = { false, false, false, false, false },
};
// clang-format on

static const char *mode_names[LM_COUNT] = {"IS", "IX", "S", "SIX", "X"};

err_t
gr_lock_init (struct gr_lock *l, const struct i_os os, error *e)
{
  const err_t result = os.table->mutex_create (os.self, &l->mutex, e);
  if (result != SUCCESS) {
    return result;
  }

  l->os = os;
  memset (l->holder_counts, 0, sizeof (l->holder_counts));
  l->head = NULL;

  return SUCCESS;
}

#ifndef NDEBUG
TEST (gr_lock_init)
{
  TEST_CASE ("mutex create green path")
  {
    struct gr_lock l;
    gr_lock_init (&l, ctx->os, &ctx->e);
    gr_lock_destroy (&l);
  }
}
#endif

void
gr_lock_destroy (struct gr_lock *l)
{
  l->mutex.table->mutex_lock (l->mutex.self);
  // TODO - Caller must ensure all threads have released locks
  // You could put a done flag - and assert !done on actions
  l->mutex.table->mutex_unlock (l->mutex.self);

  l->os.table->mutex_free (l->os.self, l->mutex.self);

  while (l->head) {
    struct gr_lock_waiter *w = l->head;
    l->head                  = w->next;
    l->os.table->cond_free (l->os.self, w->cond.self);
  }
}

#ifndef NDEBUG
TEST (gr_lock_destroy)
{
  TEST_CASE ("green path")
  {
    struct gr_lock l;
    gr_lock_init (&l, ctx->os, &ctx->e);
    gr_lock_destroy (&l);
  }
}
#endif

/**
 * Example:
 *
 * Granted Group:
 * IS IX IS IS IS
 *
 * Granted Group Count:
 * IS = 4
 * IX = 1
 *
 * Lock S is compatible?
 * IS > 0 -> IS + S = GOOD
 * IX > 1 -> IX + S = BAD
 * - Not Compatible
 *
 * Lock IS is compatible?
 * IS > 0 -> IS + IS = GOOD
 * IX > 1 -> IX + IS = GOOD
 * - Compatible
 */
static bool
is_compatible (const struct gr_lock *l, const enum lock_mode mode)
{
  for (int i = 0; i < LM_COUNT; i++) {
    if (l->holder_counts[i] > 0 && !compatible[mode][i]) {
      return false;
    }
  }
  return true;
}

#ifndef NDEBUG
TEST (gr_lock_is_compatible)
{
  struct gr_lock l;
  gr_lock_init (&l, ctx->os, &ctx->e);

  // All locks compatible on init
  test_assert (is_compatible (&l, LM_IS));
  test_assert (is_compatible (&l, LM_IX));
  test_assert (is_compatible (&l, LM_S));
  test_assert (is_compatible (&l, LM_SIX));
  test_assert (is_compatible (&l, LM_X));

  // IS is incompatible with X
  gr_lock (&l, LM_IS, &ctx->e);
  test_assert (is_compatible (&l, LM_IS));
  test_assert (is_compatible (&l, LM_IX));
  test_assert (is_compatible (&l, LM_S));
  test_assert (is_compatible (&l, LM_SIX));
  test_assert (!is_compatible (&l, LM_X));
  gr_unlock (&l, LM_IS);

  // IX is incompatible with S SIX X
  gr_lock (&l, LM_IX, &ctx->e);
  test_assert (is_compatible (&l, LM_IS));
  test_assert (is_compatible (&l, LM_IX));
  test_assert (!is_compatible (&l, LM_S));
  test_assert (!is_compatible (&l, LM_SIX));
  test_assert (!is_compatible (&l, LM_X));
  gr_unlock (&l, LM_IX);

  // S is incompatible with IX SIX X
  gr_lock (&l, LM_S, &ctx->e);
  test_assert (is_compatible (&l, LM_IS));
  test_assert (!is_compatible (&l, LM_IX));
  test_assert (is_compatible (&l, LM_S));
  test_assert (!is_compatible (&l, LM_SIX));
  test_assert (!is_compatible (&l, LM_X));
  gr_unlock (&l, LM_S);

  // SIX is incompatible with IX S SIX X
  gr_lock (&l, LM_SIX, &ctx->e);
  test_assert (is_compatible (&l, LM_IS));
  test_assert (!is_compatible (&l, LM_IX));
  test_assert (!is_compatible (&l, LM_S));
  test_assert (!is_compatible (&l, LM_SIX));
  test_assert (!is_compatible (&l, LM_X));
  gr_unlock (&l, LM_SIX);

  // X is incompatible with IS IX S SIX X
  gr_lock (&l, LM_X, &ctx->e);
  test_assert (!is_compatible (&l, LM_IS));
  test_assert (!is_compatible (&l, LM_IX));
  test_assert (!is_compatible (&l, LM_S));
  test_assert (!is_compatible (&l, LM_SIX));
  test_assert (!is_compatible (&l, LM_X));
  gr_unlock (&l, LM_X);
}
#endif

err_t
gr_lock (struct gr_lock *l, const enum lock_mode mode, error *e)
{
  // First do a global mutex lock
  l->mutex.table->mutex_lock (l->mutex.self);

  // If it's compatible - just increment mode count and move on
  if (is_compatible (l, mode)) {
    TEST_MARK ("gr_lock:gr_lock:immediate_acquire");
    goto acquire;
  }

  // Otherwise, we need to create a new lock waiter
  struct gr_lock_waiter waiter = {
      .mode = mode,
      .prev = NULL,
      .next = NULL,
  };
  if (l->os.table->cond_create (l->os.self, &waiter.cond, e)) {
    // Ok here - we just failed and everything is unlocked
    l->mutex.table->mutex_unlock (l->mutex.self);
    return error_trace (e);
  }

  // Append waiter to the linked list of waiters
  if (l->head == NULL) {
    l->head = &waiter;
  } else {
    struct gr_lock_waiter *end = l->head;
    while (end->next != NULL) {
      end = end->next;
    }

    end->next   = &waiter;
    waiter.prev = end;
  }

  // Wait for someone to signal my condition variable - main wait code
  while (!is_compatible (l, mode)) {
    TEST_MARK ("gr_lock:gr_lock:wait");
    waiter.cond.table->cond_wait (waiter.cond.self, l->mutex.self);
  }

  // Remove from waiters list
  if (waiter.prev != NULL) {
    waiter.prev->next = waiter.next;
  } else {
    ASSERT (l->head == &waiter);
    l->head = waiter.next;
  }
  if (waiter.next != NULL) {
    waiter.next->prev = waiter.prev;
  }

  // Release resources
  l->os.table->cond_free (l->os.self, waiter.cond.self);

acquire:
  // Acquire the lock
  l->holder_counts[mode]++;
  l->mutex.table->mutex_unlock (l->mutex.self);
  return SUCCESS;
}

void
gr_unlock (struct gr_lock *l, const enum lock_mode mode)
{
  l->mutex.table->mutex_lock (l->mutex.self);

  // do unlock
  ASSERT (l->holder_counts[mode] > 0);
  l->holder_counts[mode]--;

  // Wake any compatible waiters
  if (l->head) {
    for (struct gr_lock_waiter *w = l->head; w; w = w->next) {
      // signal all waiters - they do the compatability check - it's ok
      w->cond.table->cond_signal (w->cond.self);
    }
  }

  l->mutex.table->mutex_unlock (l->mutex.self);
}

#ifndef NDEBUG
struct thread_ctx
{
  struct gr_lock *l;
  enum lock_mode  mode1;
  enum lock_mode  mode2;
  _Atomic u32     locked1;
  _Atomic u32     locked2;
  _Atomic u32     gate;
};

static void *
thread1 (void *_ctx)
{
  struct thread_ctx *ctx = _ctx;

  while (!atomic_load (&ctx->gate)) {
    spin_pause ();
  }

  // This should pass through
  gr_lock (ctx->l, ctx->mode1, NULL);

  atomic_store (&ctx->locked1, 1);

  return NULL;
}

static void *
thread2 (void *_ctx)
{
  struct thread_ctx *ctx = _ctx;

  while (!atomic_load (&ctx->gate)) {
    spin_pause ();
  }

  // Wait until thread 1 issued the lock
  while (!atomic_load (&ctx->locked1)) {
    spin_pause ();
  }

  // This is the lock query in question
  gr_lock (ctx->l, ctx->mode2, NULL);

  atomic_store (&ctx->locked2, 1);

  return NULL;
}

TEST (gr_lock_unlock)
{
  i_thread       t1, t2;
  struct gr_lock l;
  gr_lock_init (&l, ctx->os, &ctx->e);

  // Cartesion product
  for (int m1 = 0; m1 < LM_COUNT; ++m1) {
    for (int m2 = 0; m2 < LM_COUNT; ++m2) {
      TEST_CASE ("%s + %s", mode_names[m1], mode_names[m2])
      {
        test_reset_marks ();

        struct thread_ctx lctx = {
            .l       = &l,
            .mode1   = m1,
            .mode2   = m2,
            .locked1 = 0,
            .locked2 = 0,
            .gate    = 0,
        };

        ctx->os.table->thread_create (ctx->os.self, &t1, thread1, &lctx, &ctx->e);
        ctx->os.table->thread_create (ctx->os.self, &t2, thread2, &lctx, &ctx->e);

        // Launch both threads
        atomic_store (&lctx.gate, 1);

        if (compatible[m1][m2]) {
          // 2 finishes without unlocking anything
          while (!atomic_load (&lctx.locked2)) {
            spin_pause ();
          }

          // 1 LOCKED
          // 2 LOCKED

          gr_unlock (&l, m1);
          gr_unlock (&l, m2);

          test_assert_mark_hit ("gr_lock:gr_lock:immediate_acquire");
          test_assert_mark_not_hit ("gr_lock:gr_lock:wait");
        } else {
          // 1 finishes fine
          while (!atomic_load (&lctx.locked1)) {
            spin_pause ();
          }

          // 1 LOCKED
          // 2 PENDING

          // Wait 10 ms and 2 is STILL not locked
          i_sleep_ms (10);
          test_assert_int_equal (atomic_load (&lctx.locked2), 0);

          // Unlock 2
          gr_unlock (&l, m1);
          while (!atomic_load (&lctx.locked2)) {
            spin_pause ();
          }

          // 1 UNLOCKED
          // 2 LOCKED

          gr_unlock (&l, m2);

          test_assert_mark_hit ("gr_lock:gr_lock:immediate_acquire");
          test_assert_mark_hit ("gr_lock:gr_lock:wait");
        }

        ctx->os.table->thread_join (ctx->os.self, t1.self);
        ctx->os.table->thread_join (ctx->os.self, t2.self);
      }
    }
  }
}
#endif

const char *
gr_lock_mode_name (const enum lock_mode mode)
{
  if (mode >= 0 && mode < LM_COUNT) {
    return mode_names[mode];
  }
  UNREACHABLE (); // LCOV_EXCL_LINE
}

#ifndef NDEBUG
// Asserted rather than logged: i_log_info is ((void)0) below LOG_INFO, which
// discards its arguments, so a logged call would not even evaluate the name.
TEST (gr_lock_mode_name)
{
  TEST_CASE ("Every mode has its own name")
  {
    test_assert_int_equal (strcmp (gr_lock_mode_name (LM_IS), "IS"), 0);
    test_assert_int_equal (strcmp (gr_lock_mode_name (LM_IX), "IX"), 0);
    test_assert_int_equal (strcmp (gr_lock_mode_name (LM_S), "S"), 0);
    test_assert_int_equal (strcmp (gr_lock_mode_name (LM_SIX), "SIX"), 0);
    test_assert_int_equal (strcmp (gr_lock_mode_name (LM_X), "X"), 0);
  }

  TEST_CASE ("The table covers the enum, with no duplicates")
  {
    for (int m = 0; m < LM_COUNT; ++m) {
      const char *name = gr_lock_mode_name ((enum lock_mode)m);
      test_assert (name != NULL);
      test_assert (name[0] != '\0');

      for (int n = 0; n < m; ++n) {
        test_assert (strcmp (name, gr_lock_mode_name ((enum lock_mode)n)) != 0);
      }
    }
  }
}
#endif

enum lock_mode
get_parent_mode (const enum lock_mode child_mode)
{
  switch (child_mode) {
    case LM_IS:
    case LM_S: {
      return LM_IS;
    }
    case LM_IX:
    case LM_SIX:
    case LM_X: {
      return LM_IX;
    }
    case LM_COUNT: {
      UNREACHABLE (); // LCOV_EXCL_LINE
    }
  }
  UNREACHABLE (); // LCOV_EXCL_LINE
}

#ifndef NDEBUG
TEST (get_parent_mode)
{
  test_assert_int_equal (get_parent_mode (LM_IS), LM_IS);
  test_assert_int_equal (get_parent_mode (LM_S), LM_IS);
  test_assert_int_equal (get_parent_mode (LM_IX), LM_IX);
  test_assert_int_equal (get_parent_mode (LM_SIX), LM_IX);
  test_assert_int_equal (get_parent_mode (LM_X), LM_IX);
}
#endif

#ifndef NDEBUG

/* --- Test Infrastructure --- */

struct lock_test_ctx
{
  struct gr_lock *lock;
  struct i_os     os;

  // Coordination Primitives
  i_mutex         gate_mtx;
  i_cond          gate_cv;
  bool            gate_open;

  // Counters and State
  atomic_int      t1_acquired;
  atomic_int      t2_blocked;
  atomic_int      t2_acquired;
  atomic_int      counter;

  // Set by the main thread when thread 1 may drop [mode1]. A gate rather
  // than a sleep: the main thread gets to sample the blocked state for as
  // long as it needs to, so a loaded machine cannot turn the sample into a
  // race.
  atomic_int      t1_release;

  enum lock_mode  mode1;
  enum lock_mode  mode2;
};

static void
lock_test_ctx_init (struct lock_test_ctx *ctx, struct gr_lock *lock, const struct i_os os)
{
  error e = error_create ();

  memset (ctx, 0, sizeof (*ctx));
  ctx->lock = lock;
  ctx->os   = os;

  // Not NULL: the OS layer reports failures through error_trace, which
  // dereferences the error
  os.table->mutex_create (os.self, &ctx->gate_mtx, &e);
  os.table->cond_create (os.self, &ctx->gate_cv, &e);
  ctx->gate_open = false;
}

static void
lock_test_ctx_destroy (struct lock_test_ctx *ctx)
{
  ctx->os.table->mutex_free (ctx->os.self, ctx->gate_mtx.self);
  ctx->os.table->cond_free (ctx->os.self, ctx->gate_cv.self);
}

/* --- Deterministic Thread Routines --- */

static void *
thread_hold_and_signal (void *arg)
{
  struct lock_test_ctx *ctx = arg;
  error                 e   = error_create ();

  // Secure the lock first
  gr_lock (ctx->lock, ctx->mode1, &e);

  // Signal to Thread 2 that the lock is held
  ctx->gate_mtx.table->mutex_lock (ctx->gate_mtx.self);
  ctx->t1_acquired = 1;
  ctx->gate_open   = true;
  ctx->gate_cv.table->cond_broadcast (ctx->gate_cv.self);
  ctx->gate_mtx.table->mutex_unlock (ctx->gate_mtx.self);

  // Hold until the main thread has sampled whatever it wanted to sample
  while (!atomic_load (&ctx->t1_release)) {
    spin_pause ();
  }

  gr_unlock (ctx->lock, ctx->mode1);
  return NULL;
}

/// Samples [l] under its own mutex while other threads are still running:
/// returns whether anyone is parked inside gr_lock and copies out the holder
/// counts that go with that answer.
///
/// A waiter is pushed onto [l->head] under [l->mutex] and only unlinked once
/// its mode became compatible, so "is anyone blocked right now" is an exact
/// answer here and not a guess about scheduling.
static bool
lock_sample (struct gr_lock *l, int *holder_counts)
{
  l->mutex.table->mutex_lock (l->mutex.self);
  const bool waiting = l->head != NULL;
  memcpy (holder_counts, l->holder_counts, sizeof (l->holder_counts));
  l->mutex.table->mutex_unlock (l->mutex.self);
  return waiting;
}

static void *
thread_wait_and_try (void *arg)
{
  struct lock_test_ctx *ctx = arg;
  error                 e   = error_create ();

  // Wait for Thread 1 to confirm it holds the lock
  ctx->gate_mtx.table->mutex_lock (ctx->gate_mtx.self);
  while (!ctx->gate_open) {
    ctx->gate_cv.table->cond_wait (ctx->gate_cv.self, ctx->gate_mtx.self);
  }
  ctx->gate_mtx.table->mutex_unlock (ctx->gate_mtx.self);

  // Attempt acquisition (will block if incompatible)
  ctx->t2_blocked = 1;
  gr_lock (ctx->lock, ctx->mode2, &e);

  ctx->t2_acquired = 1;
  ctx->t2_blocked  = 0;
  gr_unlock (ctx->lock, ctx->mode2);

  return NULL;
}

static void *
random_stress_worker (void *arg)
{
  struct lock_test_ctx *ctx  = arg;
  error                 e    = error_create ();
  uint32_t              seed = (uint32_t)(uintptr_t)arg;

  for (int i = 0; i < 1000; i++) {
    // Fast thread-local random
    seed                = seed * 1103515245 + 12345;
    enum lock_mode mode = (seed % LM_COUNT);

    gr_lock (ctx->lock, mode, &e);

    // If Exclusive or Shared-Intent-Exclusive, verify atomicity
    if (mode == LM_X || mode == LM_SIX) {
      int val = atomic_load (&ctx->counter);
      atomic_store (&ctx->counter, val + 1);
    }

    // Integrity check: current mode must have at least one holder
    if (ctx->lock->holder_counts[mode] == 0) {
      panic ("Failed test");
    }

    gr_unlock (ctx->lock, mode);
  }
  return NULL;
}

/* --- Tests --- */

TEST (gr_lock_basic_sanity)
{
  struct gr_lock lock;
  gr_lock_init (&lock, ctx->os, &ctx->e);

  for (int mode = 0; mode < LM_COUNT; mode++) {
    gr_lock (&lock, mode, &ctx->e);
    test_assert_equal (lock.holder_counts[mode], 1);
    gr_unlock (&lock, mode);
    test_assert_equal (lock.holder_counts[mode], 0);
  }
  gr_lock_destroy (&lock);
}

// Example of a Compatibility Test (Compatible)
TEST (gr_lock_is_is_compatible)
{
  struct gr_lock lock;
  gr_lock_init (&lock, ctx->os, &ctx->e);

  struct lock_test_ctx lctx;
  lock_test_ctx_init (&lctx, &lock, ctx->os);
  lctx.mode1 = LM_IS;
  lctx.mode2 = LM_IS;

  i_thread t1, t2;
  ctx->os.table->thread_create (ctx->os.self, &t1, thread_hold_and_signal, &lctx, &ctx->e);
  ctx->os.table->thread_create (ctx->os.self, &t2, thread_wait_and_try, &lctx, &ctx->e);

  // IS + IS: thread 2 gets in while thread 1 still holds the lock
  while (!atomic_load (&lctx.t2_acquired)) {
    spin_pause ();
  }
  atomic_store (&lctx.t1_release, 1);

  ctx->os.table->thread_join (ctx->os.self, t1.self);
  ctx->os.table->thread_join (ctx->os.self, t2.self);

  test_assert (lctx.t1_acquired && lctx.t2_acquired);
  lock_test_ctx_destroy (&lctx);
  gr_lock_destroy (&lock);
}

// Example of a Blocking Test (Incompatible)
//
// This used to sleep 50ms and then sample "is thread 2 still blocked?", which
// is not an invariant on a loaded machine - it failed on macOS. It now waits
// for thread 2 to actually be parked on the lock's waiter list and holds
// thread 1 until the sample is taken, so there is no timing assumption left.
TEST (gr_lock_is_x_blocks)
{
  struct gr_lock lock;
  gr_lock_init (&lock, ctx->os, &ctx->e);

  struct lock_test_ctx lctx;
  lock_test_ctx_init (&lctx, &lock, ctx->os);
  lctx.mode1 = LM_IS;
  lctx.mode2 = LM_X;

  i_thread t1, t2;
  ctx->os.table->thread_create (ctx->os.self, &t1, thread_hold_and_signal, &lctx, &ctx->e);
  ctx->os.table->thread_create (ctx->os.self, &t2, thread_wait_and_try, &lctx, &ctx->e);

  // Thread 1 holds IS until we release it, so once thread 2 is on the waiter
  // list it stays there - X is incompatible with IS
  int held[LM_COUNT];
  while (!lock_sample (&lock, held)) {
    spin_pause ();
  }

  test_assert (atomic_load (&lctx.t1_acquired));
  test_assert (atomic_load (&lctx.t2_blocked));
  test_assert (!atomic_load (&lctx.t2_acquired));

  // Thread 2 is parked, so it holds nothing - only thread 1's IS is granted
  test_assert_equal (held[LM_IS], 1);
  test_assert_equal (held[LM_X], 0);

  atomic_store (&lctx.t1_release, 1);

  ctx->os.table->thread_join (ctx->os.self, t1.self);
  ctx->os.table->thread_join (ctx->os.self, t2.self);

  test_assert (atomic_load (&lctx.t2_acquired)); // Should succeed after T1 releases

  // Both threads released what they took
  for (int m = 0; m < LM_COUNT; m++) {
    test_assert_equal (lock.holder_counts[m], 0);
  }

  lock_test_ctx_destroy (&lctx);
  gr_lock_destroy (&lock);
}

TEST (gr_lock_high_pressure_random)
{
  struct gr_lock lock;
  gr_lock_init (&lock, ctx->os, &ctx->e);

  struct lock_test_ctx lctx;
  lock_test_ctx_init (&lctx, &lock, ctx->os);

  i_thread threads[12];

  for (int i = 0; i < 12; i++) {
    ctx->os.table->thread_create (ctx->os.self, &threads[i], random_stress_worker, &lctx, &ctx->e);
  }

  for (int i = 0; i < 12; i++) {
    ctx->os.table->thread_join (ctx->os.self, threads[i].self);
  }

  // Final Validation
  for (int m = 0; m < LM_COUNT; m++) {
    test_assert_equal (lock.holder_counts[m], 0);
  }

  lock_test_ctx_destroy (&lctx);
  gr_lock_destroy (&lock);
}

#endif
