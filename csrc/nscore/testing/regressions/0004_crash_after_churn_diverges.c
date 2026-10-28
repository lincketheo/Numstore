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

#include "core/ns_stdtypes.h"
#include "numstore.h"

#ifndef NDEBUG
#  include "core/ns_testing.h"
#  include "nscore/testing/simulation/ns_numstore_simulation.h"
#endif

#ifndef NDEBUG

// Rounds of churn, and how many data operations each round runs before the
// one crash. 40 x 25 puts the run in the same range as the ns_simtest seeds
// that fail (1200-2300 steps, 30+ crashes) without taking all day.
#  define ROUNDS 40
#  define CHURN  25

/*
 * A crash loses track of data that was already auto-committed.
 *
 * Every ns_simtest seed in this family fails on a crash_and_reopen, and for
 * two of them the database was validated clean on the step immediately
 * before - so the damage appears across the crash, not during the operations
 * leading up to it. None of the small hand-written shapes reproduce it
 * (delete inside a txn then crash, names long enough to need PG_VAR_TAIL
 * overflow, 40 plain crash cycles with create/delete churn), so this drives
 * the simulation stepper and lets it build the page layout the bug needs.
 *
 * The shape is deliberate: a long run of data churn that allocates and frees
 * pages, then exactly one crash, repeated. Transactions and close_and_reopen
 * are disabled, so every operation auto-commits and the crash is the only
 * thing that can discard state - which means nothing should be lost at all.
 *
 * It fails on the crash at step 545, validating the ropes after the reopen:
 *
 *   expected page type UNKNOWN, got UNKNOWN
 *     nss_crash_and_reopen -> ns_db_validate_all -> ns_visit_variables
 *       -> ns_consume_one_var -> ns_rope_validate -> ns_rope_in_scan
 *       -> in_validate_consumer -> ns_rope_page_stats
 *
 * An inner node points at a page whose type byte is garbage, so the page was
 * freed and handed back out with the pointer left behind, or recovery never
 * restored it. Same family as apps/bugs 0002 and 0003.
 *
 * Note: after the failing step the process also trips
 *
 *   atomic_load (&os->fds[i]) == -1    (ns_posix_os.c:44)
 *
 * on teardown - file descriptors are still open when the OS layer is freed.
 * That is a second defect in the crash path's cleanup, and it aborts the
 * process, so tests ordered after this one do not run.
 *
 *      CAUSE: not yet known. This test is the reproduction, not the
 *      diagnosis - see apps/bugs/README.md and the bug scripts in that
 *      directory for the seeds that found this family.
 */
TEST (0004_crash_after_churn_diverges)
{
  // Churn that allocates and frees pages, but cannot discard state
  u8 churn[NSS_AT_LEN]                          = {0};
  churn[NSS_CREATE_AND_SWAP_IF_EMPTY]           = 1;
  churn[NSS_SWITCH]                             = 1;
  churn[NSS_DELETE_CURRENT_VARIABLE_AND_SWITCH] = 1;
  churn[NSS_INSERT]                             = 1;
  churn[NSS_REMOVE]                             = 1;
  churn[NSS_READ]                               = 1;
  churn[NSS_WRITE]                              = 1;

  // The one action each round ends on
  u8 crash[NSS_AT_LEN]                          = {0};
  crash[NSS_CRASH_AND_REOPEN]                   = 1;

  struct ns_simulation_params params            = {
      .seed              = 20261027,
      .commit_hash       = "regression-0004",
      .sequence_id       = 20261027,
      .dbname            = "test",
      .max_insert_len    = 100000,
      .max_tsize         = 4096,
      // Must stay 0, or the stepper re-randomizes the action set out from
      // under ns_simul_set_enabled
      .sample_space_prob = 0,
      .test_os           = ctx->os,
      .test_mem          = ctx->mem,
      .reliable_mem      = ctx->mem,
      .reliable_os       = ctx->os,
  };
  for (int i = 0; i < NSS_AT_LEN; ++i) {
    params.enabled[i] = churn[i];
  }

  struct ns_simulation *sim = ns_simul_open (params, &ctx->e);
  test_assert (sim != NULL);

  // Once a step fails the stepper forbids another, so stop at the first one
  // and report it below rather than asserting inside the loop
  err_t res = SUCCESS;

  for (int round = 0; round < ROUNDS && res == SUCCESS; ++round) {
    ns_simul_set_enabled (sim, churn);
    for (int i = 0; i < CHURN && res == SUCCESS; ++i) {
      res = ns_simul_step (sim, &ctx->e);
    }

    if (res == SUCCESS) {
      ns_simul_set_enabled (sim, crash);
      res = ns_simul_step (sim, &ctx->e);
    }
  }

  // This is the one that fails: churn followed by a crash has to survive
  test_assert_int_equal (res, 0);

  test_assert_int_equal (ns_simul_close (sim, &ctx->e), 0);
}

#endif
