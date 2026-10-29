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

#include "nscore/testing/simulation/ns_numstore_simulation.h"

#include "core/ns_arena_alloc.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_logging.h"
#include "core/ns_numerics.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "nscore/nsdb/ns_nsdb.h"
#include "nscore/testing/simulation/ns_db_state_machine.h"
#include "nscore/testing/simulation/ns_operation_generator.h"
#include "nscore/testing/simulation/ns_ref_state_machine.h"
#include "nscore/types/ns_types.h"

#include <string.h>

#ifndef NDEBUG
#  include "core/ns_testing.h"
#endif

struct ns_simulation
{
  struct ns_ref     *ref;
  struct ns_db      *db;

  // Which actions are turned on
  u8                 enabled[NSS_AT_LEN];

  const char        *dbname;
  b_size             max_insert_len;
  t_size             max_tsize;
  float              sample_space_prob;

  // Run identity
  u64                seed;
  const char        *commit_hash;
  u64                sequence_id;

  // Run metrics
  u64                start;       // Epoch for everything below
  u64                step_number; // Which step is the test in
  u64                clock;       // Absolute timestamp of the last observation

  // Throughput: every byte inserted, removed, read or written over the run
  u64                total_bytes_moved;

  i_timer            timer;
  struct arena_alloc alloc;

  // The OS the system under test runs on (can be faulty)
  struct i_os        test_os;

  // Memory used by the test (can be faulty)
  struct i_mem       test_mem;

  // Memory used for things that aren't being tested
  struct i_mem       reliable_mem;

  // OS used for things that aren't being tested (metrics timers and the like)
  struct i_os        reliable_os;
};

////////// ACTIONS

static err_t
nss_begin_txn (struct ns_simulation *meta, error *e)
{
  WRAP (ns_ref_begin_txn (meta->ref, e));
  WRAP (ns_db_begin_txn (meta->db, e));
  return SUCCESS;
}

static err_t
nss_commit_txn (struct ns_simulation *meta, error *e)
{
  WRAP (ns_ref_commit_txn (meta->ref, e));
  WRAP (ns_db_commit_txn (meta->db, e));
  return SUCCESS;
}

static err_t
nss_rollback_txn (struct ns_simulation *meta, error *e)
{
  ns_ref_rollback_txn (meta->ref);
  WRAP (ns_db_rollback_txn (meta->db, e));
  return SUCCESS;
}

static err_t
nss_crash_and_reopen (struct ns_simulation *meta, error *e)
{
  ns_ref_crash_and_reopen (meta->ref);
  WRAP (ns_db_crash_and_reopen (meta->db, e));

  WRAP (ns_db_validate_all (meta->db, e));
  return SUCCESS;
}

static err_t
nss_close_and_reopen (struct ns_simulation *meta, error *e)
{
  ns_ref_close_and_reopen (meta->ref);
  WRAP (ns_db_close_and_reopen (meta->db, e));

  WRAP (ns_db_validate_all (meta->db, e));
  return SUCCESS;
}

static err_t
nss_create (struct ns_simulation *meta, struct operation *op, error *e)
{
  WRAP (ns_ref_create (meta->ref, op->op_create.vname, op->op_create.t, e));
  WRAP (ns_db_create (meta->db, op->op_create.vname, op->op_create.typestr, e));
  return SUCCESS;
}

static err_t
nss_switch (struct ns_simulation *meta, struct operation *op, error *e)
{
  ns_ref_switch (meta->ref, op->op_switch.next);
  WRAP (ns_db_switch (meta->db, op->op_switch.next, e));
  return SUCCESS;
}

static err_t
nss_delete (struct ns_simulation *meta, struct operation *op, error *e)
{
  ns_ref_delete_and_switch (meta->ref, op->op_delete.next);
  WRAP (ns_db_delete_and_switch (meta->db, op->op_delete.next, e));
  return SUCCESS;
}

static err_t
nss_read_compare (
    struct ns_simulation *meta,
    void                 *ref_buf,
    void                 *db_buf,
    b_size                buf_size,
    struct stride         stride,
    error                *e
)
{
  b_size  ref_read = ns_ref_read (meta->ref, ref_buf, stride);

  sb_size db_read  = ns_db_read (meta->db, db_buf, buf_size, stride, e);
  if (db_read < 0) {
    return error_trace (e);
  }

  if (ref_read != (b_size)db_read) {
    return error_causef (e, ERR_CORRUPT, "Database read lengths don't match");
  }

  // operation ensures no overflow
  ASSERT (ref_read == stride.nelems);

  // Nothing was read, so there is nothing to compare
  if (ref_read == 0) {
    return SUCCESS;
  }

  b_size size = ref_read * ns_ref_cur_tsize (meta->ref);
  if (memcmp (db_buf, ref_buf, size)) {
    return error_causef (e, ERR_CORRUPT, "Database doesn't match reference");
  }

  return SUCCESS;
}

/**
 * Reads [start, start + nelems) with stride 1 from both, and compares.
 */
static err_t
nss_validate_range (
    struct ns_simulation *meta,
    b_size                start,
    b_size                nelems,
    u8                   *db_buf,
    u8                   *ref_buf,
    b_size                buf_size,
    error                *e
)
{
  if (nelems == 0) {
    return SUCCESS;
  }

  struct stride stride = {.start = start, .stride = 1, .nelems = nelems};

  return nss_read_compare (meta, ref_buf, db_buf, buf_size, stride, e);
}

/******************************************************************************
 *                       Whole-database validation                            *
 ******************************************************************************/

struct nss_var_check
{
  struct ns_simulation *meta;
  u32                   ndb;     // variables the database reports
  struct string         unknown; // first one the reference does not have
  bool                  has_unknown;
};

static err_t
nss_check_one_var (struct variable *v, void *ctx, error *e)
{
  struct nss_var_check *chk = ctx;
  (void)e;

  chk->ndb++;

  if (!chk->has_unknown && !ns_ref_var_exists_str (chk->meta->ref, v->vname)) {
    chk->unknown     = v->vname;
    chk->has_unknown = true;
  }

  return SUCCESS;
}

/**
 * The database's set of variables has to match the reference's after every
 * operation.
 *
 * nss_validate_after_write only checks the bytes of the current variable, so
 * a variable that one operation loses outright is not noticed until something
 * happens to touch it again - which in practice is hundreds of steps later,
 * long after the operation that caused it has scrolled out of the log.
 *
 * ns_db_visit_vars reads through the pager with no transaction, so this can
 * only run outside one.
 */
static err_t
nss_validate_variables (struct ns_simulation *meta, error *e)
{
  if (meta->ref->in_txn) {
    return SUCCESS;
  }

  struct nss_var_check chk = {.meta = meta, .ndb = 0, .has_unknown = false};
  WRAP (ns_db_visit_vars (meta->db, nss_check_one_var, &chk, e));

  u32 nref = ns_ref_nvars (meta->ref);
  if (chk.ndb != nref) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "Database holds %u variables, reference holds %u",
        chk.ndb,
        nref
    );
  }

  if (chk.has_unknown) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "Database holds variable %.*s, which the reference does not",
        strfmt (&chk.unknown)
    );
  }

  return SUCCESS;
}

static err_t
nss_validate_after_write (
    struct ns_simulation *meta,
    MAYBE_UNUSED b_size   effected_start,
    MAYBE_UNUSED b_size   effected_nelems,
    u8                   *db_buf,
    u8                   *ref_buf,
    b_size                buf_size,
    error                *e
)
{
  // Empty variable, nothing to validate
  if (ns_ref_cur_len (meta->ref) == 0) {
    return SUCCESS;
  }

  if (nss_validate_range (meta, 0, ns_ref_cur_len (meta->ref), db_buf, ref_buf, buf_size, e) < 0) {
    return error_trace (e);
  }

  return ns_db_validate (meta->db, e);
}

static err_t
nss_insert (struct ns_simulation *meta, struct operation *op, error *e)
{
  sb_size ref_inserted = ns_ref_insert (
      meta->ref,
      op->data,
      op->op_insert.ofst,
      op->op_insert.nelems,
      e
  );
  if (ref_inserted < 0) {
    return error_trace (e);
  }

  sb_size db_inserted = ns_db_insert (
      meta->db,
      op->data,
      op->data_size,
      op->op_insert.ofst,
      op->op_insert.nelems,
      e
  );
  if (db_inserted < 0) {
    return error_trace (e);
  }

  if (ref_inserted != db_inserted) {
    return error_causef (e, ERR_CORRUPT, "Database inserted lengths don't match");
  }

  ASSERT ((b_size)ref_inserted == op->op_insert.nelems);

  return nss_validate_after_write (
      meta,
      op->op_insert.ofst,
      op->op_insert.nelems,
      op->db_buf,
      op->ref_buf,
      op->buf_size,
      e
  );
}

static err_t
nss_remove (struct ns_simulation *meta, struct operation *op, error *e)
{
  b_size  ref_removed = ns_ref_remove (meta->ref, op->ref_buf, op->op_remove.str);

  sb_size db_removed  = ns_db_remove (meta->db, op->db_buf, op->buf_size, op->op_remove.str, e);
  if (db_removed < 0) {
    return error_trace (e);
  }

  if (ref_removed != (b_size)db_removed) {
    return error_causef (e, ERR_CORRUPT, "Database removed lengths don't match");
  }

  ASSERT (ref_removed == op->op_remove.str.nelems);

  // Zero-length removal: nothing was removed, nothing to compare or revalidate
  if (ref_removed == 0) {
    return SUCCESS;
  }

  // Check that the removed data was the same
  b_size size = ref_removed * ns_ref_cur_tsize (meta->ref);
  if (memcmp (op->db_buf, op->ref_buf, size)) {
    return error_causef (e, ERR_CORRUPT, "Database doesn't match reference");
  }

  // Variable is now empty
  b_size len = ns_ref_cur_len (meta->ref);
  if (len == 0) {
    return SUCCESS;
  }

  b_size start = op->op_remove.str.start;
  if (op->op_remove.str.stride == 0 || start >= len) {
    return SUCCESS;
  }

  return nss_validate_after_write (
      meta,
      start,
      len - start,
      op->db_buf,
      op->ref_buf,
      op->buf_size,
      e
  );
}

static err_t
nss_read (struct ns_simulation *meta, struct operation *op, error *e)
{
  return nss_read_compare (meta, op->ref_buf, op->db_buf, op->buf_size, op->op_read.str, e);
}

static err_t
nss_write (struct ns_simulation *meta, struct operation *op, error *e)
{
  b_size  ref_written = ns_ref_write (meta->ref, op->data, op->op_write.str);

  sb_size db_written  = ns_db_write (meta->db, op->data, op->data_size, op->op_write.str, e);
  if (db_written < 0) {
    return error_trace (e);
  }

  if (ref_written != (b_size)db_written) {
    return error_causef (e, ERR_CORRUPT, "Database written lengths don't match");
  }

  ASSERT (ref_written == op->op_write.str.nelems);

  // A strided write touches start, start + stride, ...; read the whole span
  // (with stride 1) so every touched element is covered
  b_size span = op->op_write.str.nelems == 0
                    ? 0
                    : (op->op_write.str.nelems - 1) * op->op_write.str.stride + 1;

  return nss_validate_after_write (
      meta,
      op->op_write.str.start,
      span,
      op->db_buf,
      op->ref_buf,
      op->buf_size,
      e
  );
}

////////// LOGGING

static void
format_quoted_str (char *buf, size_t bufsize, const char *str)
{
  size_t maxlen    = bufsize - 6; /* was -5: off by one, ate the closing quote */
  size_t len       = strlen (str);
  bool   truncated = len > maxlen;
  snprintf (
      buf,
      bufsize,
      "\"%.*s%s\"",
      (int)(truncated ? maxlen : len),
      str,
      truncated ? "..." : ""
  );
}

static void
nss_log_operation (struct ns_simulation *meta, struct operation *op, bool completed)
{
  const char *completed_str = completed ? "true" : "false";
  const char *in_txn_str    = meta->ref->in_txn ? "true" : "false";
  const bool  has_var       = ns_ref_nvars (meta->ref) > 0;

  print_json_start ();

  switch (op->type) {
    case NSS_BEGIN_TXN: print_entry ("action", "\"begin_txn\""); break;
    case NSS_COMMIT_TXN: print_entry ("action", "\"commit_txn\""); break;
    case NSS_ROLLBACK_TXN: print_entry ("action", "\"rollback_txn\""); break;
    case NSS_CRASH_AND_REOPEN: print_entry ("action", "\"crash_and_reopen\""); break;
    case NSS_CLOSE_AND_REOPEN: print_entry ("action", "\"close_and_reopen\""); break;
    case NSS_CREATE_AND_SWAP_IF_EMPTY: print_entry ("action", "\"create\""); break;
    case NSS_SWITCH: print_entry ("action", "\"switch\""); break;
    case NSS_DELETE_CURRENT_VARIABLE_AND_SWITCH: print_entry ("action", "\"delete\""); break;
    case NSS_INSERT: print_entry ("action", "\"insert\""); break;
    case NSS_REMOVE: print_entry ("action", "\"remove\""); break;
    case NSS_READ: print_entry ("action", "\"read\""); break;
    case NSS_WRITE: print_entry ("action", "\"write\""); break;
    case NSS_NONE_AVAILABLE: print_entry ("action", "\"none_available\""); break;
    case NSS_AT_LEN: UNREACHABLE (); break;
  }

  print_entry ("completed", completed_str);
  print_entry ("in_txn", in_txn_str);

  // Step number
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%" PRIu64, meta->step_number);
    print_entry ("step", buf);
  }

  // Seed
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%" PRIu64, meta->seed);
    print_entry ("seed", buf);
  }

  // Commit Hash
  {
    char buf[256];
    format_quoted_str (buf, sizeof (buf), meta->commit_hash);
    print_entry ("commit_hash", buf);
  }

  // Sequence Id
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%" PRIu64, meta->sequence_id);
    print_entry ("sequence_id", buf);
  }

  // Database Name
  {
    char buf[256];
    format_quoted_str (buf, sizeof (buf), meta->dbname);
    print_entry ("dbname", buf);
  }

  // Max type size
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%d", meta->max_tsize);
    print_entry ("max_tsize", buf);
  }

  // Max Insert Length
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%" PRIu64, (u64)meta->max_insert_len);
    print_entry ("max_insert_len", buf);
  }

  // Sample Space Probability
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%g", (double)meta->sample_space_prob);
    print_entry ("sample_space_prob", buf);
  }

  // Operation Duration Ms
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%.6f", (double)meta->db->prev_op_duration_ns / 1e6);
    print_entry ("op_duration_ms", buf);
  }

  // Total Working Ms
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%.6f", (double)meta->db->total_working_ns / 1e6);
    print_entry ("total_working_ms", buf);
  }

  // Database Size
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%.9f", (double)meta->db->db_size_bytes / 1e9);
    print_entry ("db_size_gb", buf);
  }

  // Elapsed time
  {
    char buf[32];
    snprintf (
        buf,
        sizeof (buf),
        "%.6f",
        (double)(meta->timer.table->timer_now_ns (meta->timer.self) - meta->start) / 1e6
    );
    print_entry ("elapsed_ms", buf);
  }

  // number of variables
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%" PRIu32, ns_ref_nvars (meta->ref));
    print_entry ("nvars", buf);
  }

  // Tracked gb
  {
    char buf[32];
    snprintf (buf, sizeof (buf), "%.9f", (double)ns_ref_tracked_bytes (meta->ref) / 1e9);
    print_entry ("tracked_gb", buf);
  }

  if (has_var) {
    // Current variable name
    {
      char buf[256];
      format_quoted_str (buf, sizeof (buf), ns_ref_cur_name (meta->ref));
      print_entry ("cur_var", buf);
    }

    // Current variable length
    {
      char buf[32];
      snprintf (buf, sizeof (buf), "%" PRb_size, ns_ref_cur_len (meta->ref));
      print_entry ("cur_var_len", buf);
    }

    // Current variable tsize
    {
      char buf[32];
      snprintf (buf, sizeof (buf), "%" PRt_size, ns_ref_cur_tsize (meta->ref));
      print_entry ("cur_var_tsize", buf);
    }
  }

  switch (op->type) {
    case NSS_BEGIN_TXN:
    case NSS_COMMIT_TXN:
    case NSS_ROLLBACK_TXN:
    case NSS_CRASH_AND_REOPEN:
    case NSS_CLOSE_AND_REOPEN:
    case NSS_NONE_AVAILABLE: {
      break;
    }

    case NSS_CREATE_AND_SWAP_IF_EMPTY: {
      // Variable name
      {
        char buf[256];
        format_quoted_str (buf, sizeof (buf), op->op_create.vname);
        print_entry ("vname", buf);
      }

      // Variable type
      {
        char buf[256];
        format_quoted_str (buf, sizeof (buf), op->op_create.typestr);
        print_entry ("type", buf);
      }
      break;
    }

    case NSS_SWITCH: {
      // Variable name
      {
        char buf[256];
        format_quoted_str (buf, sizeof (buf), op->op_switch.next);
        print_entry ("vname", buf);
      }
      break;
    }

    case NSS_DELETE_CURRENT_VARIABLE_AND_SWITCH: {
      // Delete current variable
      {
        char        buf[256];
        const char *val = "null";
        if (op->op_delete.next) {
          format_quoted_str (buf, sizeof (buf), op->op_delete.next);
          val = buf;
        }
        print_entry ("next", val);
      }
      break;
    }

    case NSS_INSERT:
    case NSS_REMOVE:
    case NSS_READ:
    case NSS_WRITE: {
      u64 bytes_moved = 0;
      if (has_var) {
        t_size tsize = ns_ref_cur_tsize (meta->ref);
        switch (op->type) {
          case NSS_INSERT: bytes_moved = op->op_insert.nelems * tsize; break;
          case NSS_REMOVE: bytes_moved = op->op_remove.str.nelems * tsize; break;
          case NSS_READ: bytes_moved = op->op_read.str.nelems * tsize; break;
          case NSS_WRITE: bytes_moved = op->op_write.str.nelems * tsize; break;
          default: break;
        }
      }
      meta->total_bytes_moved += bytes_moved;

      const double op_ms    = (double)meta->db->prev_op_duration_ns / 1e6;
      const double total_ms = (double)meta->db->total_working_ns / 1e6;

      if (op->type == NSS_INSERT) {
        // Offset
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_insert.ofst);
          print_entry ("ofst", buf);
        }
        // Number of elements
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_insert.nelems);
          print_entry ("nelems", buf);
        }
      } else if (op->type == NSS_REMOVE) {
        // Start
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_remove.str.start);
          print_entry ("start", buf);
        }
        // stride
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_remove.str.stride);
          print_entry ("stride", buf);
        }
        // nelems
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_remove.str.nelems);
          print_entry ("nelems", buf);
        }
      } else if (op->type == NSS_READ) {
        // start
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_read.str.start);
          print_entry ("start", buf);
        }

        // stride
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_read.str.stride);
          print_entry ("stride", buf);
        }

        // nelems
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_read.str.nelems);
          print_entry ("nelems", buf);
        }
      } else if (op->type == NSS_WRITE) { /* NSS_WRITE */

        // start
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_write.str.start);
          print_entry ("start", buf);
        }

        // stride
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_write.str.stride);
          print_entry ("stride", buf);
        }

        // nelems
        {
          char buf[32];
          snprintf (buf, sizeof (buf), "%" PRb_size, op->op_write.str.nelems);
          print_entry ("nelems", buf);
        }
      } else {
        UNREACHABLE ();
      }

      // bytes moved
      {
        char buf[32];
        snprintf (buf, sizeof (buf), "%" PRIu64, bytes_moved);
        print_entry ("bytes_moved", buf);
      }

      // bytes per ms
      {
        char buf[32];
        if (op_ms > 0) {
          snprintf (buf, sizeof (buf), "%.3f", (double)bytes_moved / op_ms);
        } else {
          snprintf (buf, sizeof (buf), "null");
        }
        print_entry ("bytes_per_ms", buf);
      }

      // total bytes moved
      {
        char buf[32];
        snprintf (buf, sizeof (buf), "%.9f", meta->total_bytes_moved / 1e9);
        print_entry ("total_gb_moved", buf);
      }

      // average bytes moved per ms
      {
        char buf[32];
        if (total_ms > 0) {
          snprintf (buf, sizeof (buf), "%.3f", (double)meta->total_bytes_moved / total_ms);
        } else {
          snprintf (buf, sizeof (buf), "null");
        }
        print_entry ("avg_bytes_per_ms", buf);
      }
      break;
    }

    case NSS_AT_LEN: UNREACHABLE (); break;
  }

  print_last_entry ("placeholder", "null");

  print_json_end ();
}

/******************************************************************************
 * SECTION: Main Api
 ******************************************************************************/

DEFINE_DBG_ASSERT (struct ns_simulation_params, ns_simulation_params, p, {
  ASSERT (p);
  ASSERT (p->commit_hash);
  ASSERT (p->dbname);
  ASSERT (p->max_insert_len > 0);
  ASSERT (p->max_tsize > 0);
  ASSERT (p->sample_space_prob <= 1);
  ASSERT (p->sample_space_prob >= 0);
})

struct ns_simulation *
ns_simul_open (struct ns_simulation_params params, error *e)
{
  DBG_ASSERT (ns_simulation_params, &params);

  // Clean up the database before starting
  if (nsdb_cleanup (params.dbname, params.reliable_os, e) < 0) {
    return NULL;
  }

  struct ns_simulation *ret = params.reliable_mem.table
                                  ->malloc (params.reliable_mem.self, 1, sizeof *ret, e);
  if (ret == NULL) {
    return NULL;
  }

  struct ns_ref *ref = ns_ref_new (default_mem (), e);
  if (ref == NULL) {
    params.reliable_mem.table->free (params.reliable_mem.self, ret);
    return NULL;
  }

  struct ns_db *db = ns_db_new (
      params.reliable_mem,
      params.reliable_os,
      params.test_mem,
      params.test_os,
      params.dbname,
      e
  );
  if (db == NULL) {
    ns_ref_free (ref);
    params.reliable_mem.table->free (params.reliable_mem.self, ret);
    return NULL;
  }

  *ret = (struct ns_simulation){
      .ref               = ref,
      .db                = db,

      .dbname            = params.dbname,
      .max_insert_len    = params.max_insert_len,
      .max_tsize         = params.max_tsize,
      .sample_space_prob = params.sample_space_prob,

      .seed              = params.seed,
      .commit_hash       = params.commit_hash,
      .sequence_id       = params.sequence_id,

      .start             = 0,
      .step_number       = 0,
      .clock             = 0,
      .total_bytes_moved = 0,

      .reliable_mem      = params.reliable_mem,
      .reliable_os       = params.reliable_os,
      .test_mem          = params.test_mem,
      .test_os           = params.test_os,
  };

  memcpy (ret->enabled, params.enabled, sizeof (params.enabled));

  // Metrics run off the reliable OS - a faulty timer would only make the
  // numbers lie
  if (params.reliable_os.table->timer_create (params.reliable_os.self, &ret->timer, e)) {
    ns_db_close (db, e);
    ns_ref_free (ref);
    params.reliable_mem.table->free (params.reliable_mem.self, ret);
    return NULL;
  }

  ret->start = ret->timer.table->timer_now_ns (ret->timer.self);
  ret->clock = ret->start;

  return ret;
}

err_t
ns_simul_close (struct ns_simulation *meta, error *e)
{
  // If we're in a transaction - we need to finish
  // that transaction some how
  if (meta->ref->in_txn) {
    nss_commit_txn (meta, e);
  }
  ns_ref_free (meta->ref);
  ns_db_close (meta->db, e);
  meta->reliable_os.table->timer_free (meta->reliable_os.self, meta->timer.self);
  meta->reliable_mem.table->free (meta->reliable_mem.self, meta);
  return error_trace (e);
}

void
ns_simul_set_enabled (struct ns_simulation *meta, const u8 enabled[NSS_AT_LEN])
{
  ASSERT (meta);
  memcpy (meta->enabled, enabled, sizeof (meta->enabled));
}

err_t
ns_simul_step (struct ns_simulation *meta, error *e)
{
  if (meta->step_number == 373) {
    printf ("HERE\n");
  }

  struct rand_op_params params = {
      .ref        = meta->ref,
      .enabled    = meta->enabled,
      .max_nelems = meta->max_insert_len,
      .max_tsize  = meta->max_tsize,
      .mem        = meta->reliable_mem,
  };

  struct operation *op = opg_random (params, e);
  if (op == NULL) {
    return error_trace (e);
  }

  err_t result = SUCCESS;
  switch (op->type) {
    case NSS_BEGIN_TXN: result = nss_begin_txn (meta, e); break;
    case NSS_COMMIT_TXN: result = nss_commit_txn (meta, e); break;
    case NSS_ROLLBACK_TXN: result = nss_rollback_txn (meta, e); break;
    case NSS_CRASH_AND_REOPEN: result = nss_crash_and_reopen (meta, e); break;
    case NSS_CLOSE_AND_REOPEN: result = nss_close_and_reopen (meta, e); break;
    case NSS_CREATE_AND_SWAP_IF_EMPTY: result = nss_create (meta, op, e); break;
    case NSS_SWITCH: result = nss_switch (meta, op, e); break;
    case NSS_DELETE_CURRENT_VARIABLE_AND_SWITCH: result = nss_delete (meta, op, e); break;
    case NSS_INSERT: result = nss_insert (meta, op, e); break;
    case NSS_REMOVE: result = nss_remove (meta, op, e); break;
    case NSS_READ: result = nss_read (meta, op, e); break;
    case NSS_WRITE: result = nss_write (meta, op, e); break;
    case NSS_NONE_AVAILABLE: break;
    default: UNREACHABLE (); return -1;
  }

  // Every operation, not just the writes: this is what catches a variable
  // going missing at the step that lost it
  if (result >= SUCCESS) {
    result = nss_validate_variables (meta, e);
  }

  nss_log_operation (meta, op, result >= SUCCESS);

  // Increment step
  meta->step_number++;

  // Choose a set of randomized actions
  if (op->type == NSS_NONE_AVAILABLE || randf () <= meta->sample_space_prob) {
    opg_spin_enabled (meta->enabled);
  }

  opg_free (op);

  if (result < SUCCESS) {
    return error_trace (e);
  }

  return SUCCESS;
}

#ifndef NDEBUG
TEST (ns_simul)
{
  TEST_CASE ("Smoke test")
  {
    struct ns_simulation_params params = {
        .seed              = 1234,
        .commit_hash       = "abcd",
        // .enabled
        .sequence_id       = 10,
        .dbname            = "foo",
        .max_insert_len    = 1000,
        .max_tsize         = 1000,
        .sample_space_prob = 1,
        .test_os           = ctx->os,
        .test_mem          = ctx->mem,
        .reliable_mem      = ctx->mem,
        .reliable_os       = ctx->os,
    };
    struct ns_simulation *simul = ns_simul_open (params, &ctx->e);

    ns_simul_step (simul, &ctx->e);
    ns_simul_step (simul, &ctx->e);
    ns_simul_step (simul, &ctx->e);

    ns_simul_close (simul, &ctx->e);
  }
}
#endif
