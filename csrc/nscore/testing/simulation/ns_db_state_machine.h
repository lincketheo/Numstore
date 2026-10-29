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

#ifndef NS_ACTUAL_DB_STEPPER
#define NS_ACTUAL_DB_STEPPER

#include "core/ns_error.h"
#include "core/ns_stdtypes.h"
#include "core/ns_stride.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "nscore/algorithms/var/ns_var_algorithms.h"

/**
 * A database stepper is a little state machine that
 * holds onto a variable and operates a bunch of operations
 * on that variable
 */
struct ns_db
{
  struct nsdb   *db; // The system under test
  struct txn    *tx; // The active transaction

  char          *var_committed; // Current variable we act on
  char          *var_working;   // For rollback

  struct i_mem   reliable_mem; // Memory used that isn't supposed to fail
  struct i_os    reliable_os;  // OS used that isn't supposed to fail

  // Metrics
  struct i_timer timer;               // timer used to record metrics
  u64            total_working_ns;    // Total time spent working
  u64            prev_op_duration_ns; // Time it took to execute the previous operation
  u64            db_size_bytes;       // Total size of the database in bytes

  // Open parameters
  const char    *dbname;   // Name of the database
  struct i_mem   test_mem; // Memory used in test - can fail
  struct i_os    test_os;  // OS the system under test runs on - can fail
};

// Create a new simulation
struct ns_db *ns_db_new (
    struct i_mem reliable_mem,
    struct i_os  reliable_os,
    struct i_mem test_mem,
    struct i_os  test_os,
    const char  *dbname,
    error       *e
);
err_t ns_db_close (struct ns_db *db, error *e);

// State Machine Actions
err_t ns_db_begin_txn (struct ns_db *db, error *e);
err_t ns_db_rollback_txn (struct ns_db *db, error *e);
err_t ns_db_commit_txn (struct ns_db *db, error *e);
err_t ns_db_crash_and_reopen (struct ns_db *db, error *e);
err_t ns_db_close_and_reopen (struct ns_db *db, error *e);
err_t ns_db_create (struct ns_db *db, const char *vname, const char *typestr, error *e);
err_t ns_db_switch (struct ns_db *db, const char *next, error *e);
err_t ns_db_delete_and_switch (struct ns_db *db, const char *next, error *e);
sb_size ns_db_insert (
    struct ns_db *db,
    const void   *data,
    b_size        dlen,
    b_size        ofst,
    b_size        nelems,
    error        *e
);
sb_size ns_db_remove (struct ns_db *db, void *dest, b_size dlen, struct stride str, error *e);
sb_size ns_db_read (struct ns_db *db, void *dest, b_size dlen, struct stride str, error *e);
sb_size ns_db_write (struct ns_db *db, const void *data, b_size dlen, struct stride str, error *e);
err_t ns_db_validate (struct ns_db *db, error *e);

// TODO - make these better
err_t ns_db_visit_vars (struct ns_db *db, var_consumer cb, void *ctx, error *e);
err_t ns_db_validate_all (struct ns_db *db, error *e);

#endif
