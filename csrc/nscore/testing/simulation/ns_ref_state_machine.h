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

#ifndef NS_REFERENCE_DB_STEPPER
#define NS_REFERENCE_DB_STEPPER

#include "core/ns_error.h"
#include "core/ns_stdtypes.h"
#include "nscore/types/ns_types.h"

/**
 * A reference stepper - uses in memory data
 * to mock the database out - this one is accurate
 * and keeps track of various metrics
 */
struct db_state
{
  struct mem_vhmap     *db_data;       // The in memory database version
  struct var_with_data *cur;           // the current variable and data
  u32                   nvars;         // Number of variables
  u32                   tracked_bytes; // Number of bytes tracked in the database
  struct i_mem          mem;           // Memory used - shouldn't fail
};

struct ns_ref
{
  struct db_state *committed; // Committed database
  struct db_state *working;   // Null if not in a transaction
  struct i_mem     mem;       // The memory used
  bool             in_txn;    // Are we in a transaction
};

// Lifecycle
struct ns_ref *ns_ref_new (struct i_mem, error *e);
void ns_ref_free (struct ns_ref *ref);

// Utils
u32 ns_ref_nvars (struct ns_ref *ref);
u32 ns_ref_tracked_bytes (struct ns_ref *ref);
const char *ns_ref_cur_name (struct ns_ref *ref);
b_size ns_ref_cur_len (struct ns_ref *ref);
t_size ns_ref_cur_tsize (struct ns_ref *ref);
bool ns_ref_var_exists (struct ns_ref *ref, const char *name);

/**
 * As ns_ref_var_exists, for a name that is not null terminated -- a variable
 * name read back out of the database is a struct string, not a C string.
 */
bool ns_ref_var_exists_str (struct ns_ref *ref, struct string name);
const char *ns_ref_random_var (struct ns_ref *ref);

// State Machine Actions
err_t ns_ref_begin_txn (struct ns_ref *ref, error *e);
void ns_ref_rollback_txn (struct ns_ref *ref);
err_t ns_ref_commit_txn (struct ns_ref *ref, error *e);
void ns_ref_crash_and_reopen (struct ns_ref *ref);
void ns_ref_close_and_reopen (struct ns_ref *ref);
err_t ns_ref_create (struct ns_ref *ref, const char *vname, struct type *type, error *e);
void ns_ref_switch (struct ns_ref *ref, const char *next);
void ns_ref_delete_and_switch (struct ns_ref *ref, const char *next);
sb_size ns_ref_insert (struct ns_ref *ref, void *data, b_size ofst, b_size len, error *e);
b_size ns_ref_remove (struct ns_ref *ref, void *dest, struct stride str);
b_size ns_ref_read (struct ns_ref *ref, void *dest, struct stride str);
b_size ns_ref_write (struct ns_ref *ref, void *data, struct stride str);

#endif
