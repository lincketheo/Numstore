#include "core/ns_periodic_task.h"

#include "ns_error.h"

#ifndef NDEBUG
#  include "core/ns_testing.h" // TEST
#endif

/******************************************************************************
 * SECTION: Periodic Task
 ******************************************************************************/

err_t
periodic_task_init (struct periodic_task *t, const struct i_os os, error *e)
{
  t->os             = os;
  t->stop           = false;
  t->wake_requested = false;
  t->done           = false;
  t->running        = false;

  if (t->os.table->mutex_create (t->os.self, &t->mutex, e)) {
    goto theend;
  }
  if (t->os.table->cond_create (t->os.self, &t->wake_cond, e)) {
    goto fail_mutex;
  }
  if (t->os.table->cond_create (t->os.self, &t->done_cond, e)) {
    goto fail_wake_cond;
  }

  goto theend;

  // Commented this out - I am pretty sure I can - but it was on a whim
  // t->os.table->cond_free (t->os.self, t->done_cond.self);
fail_wake_cond:
  t->os.table->cond_free (t->os.self, t->wake_cond.self);
fail_mutex:
  t->os.table->mutex_free (t->os.self, t->mutex.self);
theend:
  return error_trace (e);
}

static void *
periodic_task_thread (void *_ctx)
{
  struct periodic_task *t = _ctx;

  while (true) {
    t->mutex.table->mutex_lock (t->mutex.self);
    // TODO - spurrious wakeups
    if (!t->wake_requested && !t->stop) {
      t->wake_cond.table->cond_timed_wait (t->wake_cond.self, t->mutex.self, t->msec);
    }
    t->wake_requested = false;
    bool should_stop  = t->stop;
    t->mutex.table->mutex_unlock (t->mutex.self);

    if (should_stop) {
      break;
    }

    t->fn (t->ctx);
  }

  t->mutex.table->mutex_lock (t->mutex.self);
  t->done = true;
  t->done_cond.table->cond_signal (t->done_cond.self);
  t->mutex.table->mutex_unlock (t->mutex.self);

  return NULL;
}

err_t
periodic_task_start (struct periodic_task *t, u64 msec, periodic_task_fn fn, void *ctx, error *e)
{
  t->msec = msec;
  t->fn   = fn;
  t->ctx  = ctx;

  if (t->os.table->thread_create (t->os.self, &t->thread, periodic_task_thread, t, e)) {
    return error_trace (e);
  }

  t->running = true;

  return SUCCESS;
}

err_t
periodic_task_stop (struct periodic_task *t, error *e)
{
  if (!t->running) {
    return SUCCESS;
  }

  t->mutex.table->mutex_lock (t->mutex.self);
  t->stop = true;
  t->wake_cond.table->cond_signal (t->wake_cond.self);
  t->mutex.table->mutex_unlock (t->mutex.self);

  t->mutex.table->mutex_lock (t->mutex.self);
  while (!t->done) {
    t->done_cond.table->cond_wait (t->done_cond.self, t->mutex.self);
  }
  t->mutex.table->mutex_unlock (t->mutex.self);

  t->os.table->thread_join (t->os.self, t->thread.self);
  t->os.table->cond_free (t->os.self, t->done_cond.self);
  t->os.table->cond_free (t->os.self, t->wake_cond.self);
  t->os.table->mutex_free (t->os.self, t->mutex.self);
  t->running = false;

  return error_trace (e);
}

#ifndef NDEBUG

/******************************************************************************
 * SECTION: Tests
 ******************************************************************************/

struct ptask_test_ctx
{
  _Atomic int calls;
};

static void
ptask_test_fn (void *_ctx)
{
  struct ptask_test_ctx *c = _ctx;
  atomic_fetch_add (&c->calls, 1);
}

/// periodic_task_init makes exactly three fallible OS calls, in this order:
///
///   call 0        call 1              call 2
///   mutex_create  cond_create(wake)   cond_create(done)
///
/// The faulty OS budget counts calls of any kind, so a budget of N lands the
/// injected fault on call N. Each row below fails one of them and the unwind
/// has to release exactly the handles created before it - no more (double
/// free) and no fewer (leak).
///
/// A failed init must also leave the task not running, so a caller that
/// unconditionally calls periodic_task_stop on the way out does not touch a
/// mutex that was never created.
#  define PTASK_INIT_FAULT_CASE(budget, which)                              \
    do {                                                                    \
      TEST_CASE ("init fails at call %d (%s)", budget, which)               \
      {                                                                     \
        struct periodic_task t;                                             \
                                                                            \
        faulty_os_fail_after (ctx->faulty_os, budget);                      \
        test_assert (periodic_task_init (&t, ctx->faulty_os, &ctx->e) < 0); \
        test_assert (ctx->e.cause_code < 0);                                \
        error_reset (&ctx->e);                                              \
                                                                            \
        test_fail_if (t.running);                                           \
        test_assert_int_equal (periodic_task_stop (&t, &ctx->e), SUCCESS);  \
        test_assert_int_equal (ctx->e.cause_code, SUCCESS);                 \
      }                                                                     \
    }                                                                       \
    while (0)

TEST (periodic_task_init_unwinds_on_os_failure)
{
  PTASK_INIT_FAULT_CASE (0, "mutex_create");
  PTASK_INIT_FAULT_CASE (1, "cond_create wake");
  PTASK_INIT_FAULT_CASE (2, "cond_create done");

  TEST_CASE ("init succeeds once the budget is past all three calls")
  {
    struct periodic_task t;

    faulty_os_fail_after (ctx->faulty_os, 3);
    test_assert_int_equal (periodic_task_init (&t, ctx->faulty_os, &ctx->e), SUCCESS);
    test_fail_if (t.running);

    // The budget now lands on thread_create, the next fallible call.
    struct ptask_test_ctx tc = {.calls = 0};
    test_assert (periodic_task_start (&t, 1, ptask_test_fn, &tc, &ctx->e) < 0);
    test_fail_if (t.running);
    test_assert_int_equal (atomic_load (&tc.calls), 0);
    error_reset (&ctx->e);

    // TODO - start failing here leaks the mutex and both conds: stop() early
    // returns on !running, and nothing else can reach them. Needs a
    // periodic_task_free, or stop() has to tear down an inited task.
    test_assert_int_equal (periodic_task_stop (&t, &ctx->e), SUCCESS);
  }
}

/// init -> start -> (fn runs on the timer) -> stop. By the time stop returns
/// the worker is parked on done_cond, so joining cannot hang and fn cannot run
/// again.
TEST (periodic_task_start_stop)
{
  struct periodic_task  t;
  struct ptask_test_ctx tc = {.calls = 0};

  test_assert_int_equal (periodic_task_init (&t, ctx->os, &ctx->e), SUCCESS);
  test_assert_int_equal (periodic_task_start (&t, 1, ptask_test_fn, &tc, &ctx->e), SUCCESS);
  test_assert (t.running);

  // Wait for the first tick rather than sleeping a fixed amount, so a slow
  // machine does not turn into a failure.
  for (u32 i = 0; i < 2000 && atomic_load (&tc.calls) == 0; ++i) {
    i_sleep_ms (1);
  }
  test_assert (atomic_load (&tc.calls) > 0);

  test_assert_int_equal (periodic_task_stop (&t, &ctx->e), SUCCESS);
  test_fail_if (t.running);
  test_assert (t.done);

  // Nothing runs after stop returns.
  const int after = atomic_load (&tc.calls);
  i_sleep_ms (20);
  test_assert_int_equal (atomic_load (&tc.calls), after);

  TEST_CASE ("stop is idempotent")
  {
    test_assert_int_equal (periodic_task_stop (&t, &ctx->e), SUCCESS);
    test_assert_int_equal (ctx->e.cause_code, SUCCESS);
  }
}

#endif // NDEBUG
