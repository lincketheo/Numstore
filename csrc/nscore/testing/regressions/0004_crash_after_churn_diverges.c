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

/**
 * This test is meant to mimic the crash and reopen bug
 * and should be made more reliable once root cause is found
 *
 * This is not a finalized test
 */
TEST_DISABLED (0004_crash_after_churn_diverges)
{
  // Phaes 1 - churn - these operations are safe
  u8 churn[NSS_AT_LEN]                          = {0};
  churn[NSS_CREATE_AND_SWAP_IF_EMPTY]           = 1;
  churn[NSS_SWITCH]                             = 1;
  churn[NSS_DELETE_CURRENT_VARIABLE_AND_SWITCH] = 1;
  churn[NSS_INSERT]                             = 1;
  churn[NSS_REMOVE]                             = 1;
  churn[NSS_READ]                               = 1;
  churn[NSS_WRITE]                              = 1;

  // Phase 2 - crash and reopen - this operation isn't safe
  u8 crash[NSS_AT_LEN]                          = {0};
  crash[NSS_CRASH_AND_REOPEN]                   = 1;

  struct ns_simulation_params params            = {
      .seed              = 20261027,
      .commit_hash       = "regression-0004",
      .sequence_id       = 20261027,
      .dbname            = "test",
      .max_insert_len    = 100000,
      .max_tsize         = 4096,
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

  for (int round = 0; round < 40; ++round) {
    // Phase 1 - churn
    ns_simul_set_enabled (sim, churn);
    for (int i = 0; i < 25; ++i) {
      test_assert_int_equal (ns_simul_step (sim, &ctx->e), SUCCESS);
    }

    // Phase 2 - crash - this should crash
    ns_simul_set_enabled (sim, crash);
    test_assert_int_equal (ns_simul_step (sim, &ctx->e), SUCCESS);
  }

  test_assert_int_equal (ns_simul_close (sim, &ctx->e), 0);
}

#endif
