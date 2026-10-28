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

#ifndef NSS_SWARM_TEST_FIXTURE_H
#define NSS_SWARM_TEST_FIXTURE_H

#include "core/ns_stdtypes.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "nscore/testing/simulation/ns_operation_generator.h"

struct ns_simulation_params
{
  u64          seed;
  const char  *commit_hash;
  u8           enabled[NSS_AT_LEN];
  u64          sequence_id;
  const char  *dbname;
  b_size       max_insert_len;
  t_size       max_tsize;
  float        sample_space_prob;
  // The OS the system under test runs on (can be faulty)
  struct i_os  test_os;

  // Memory used by the test (can be faulty)
  struct i_mem test_mem;

  // Memory used for things that aren't being tested
  struct i_mem reliable_mem;

  // OS used for things that aren't being tested (metrics timers and the like)
  struct i_os  reliable_os;
};

// Open a new simulation
struct ns_simulation *ns_simul_open (struct ns_simulation_params params, error *e);

// Close a simulation
// Can error on ns_db_close
err_t ns_simul_close (struct ns_simulation *meta, error *e);

/*
 * Pick a random allowed action, decide its parameters, execute it against
 * both the real db and the reference model, and verify the result.
 *
 * Returns 0 on success, -1 on failure.
 *
 * once a step returns -1, the caller must not call ns_simul_step
 * again - only ns_simul_close.
 *
 * A failure here means an invalid database state - e.g. a bug
 */
err_t ns_simul_step (struct ns_simulation *meta, error *e);

/**
 * Replace the set of actions ns_simul_step is allowed to pick from.
 *
 * Lets a caller shape the action sequence instead of taking it at random,
 * which is how a regression pins down an interleaving that only shows up
 * after a particular kind of churn. Note that ns_simul_step re-randomizes the
 * set itself when no action is available or at sample_space_prob, so keep
 * that probability at 0 if the set has to stay put.
 */
void ns_simul_set_enabled (struct ns_simulation *meta, const u8 enabled[NSS_AT_LEN]);

#endif // NSS_SWARM_TEST_FIXTURE_H
