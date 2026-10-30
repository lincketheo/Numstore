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

#include "nscore/wal/ns_wal_istream.h"

#include "core/ns_crc.h"
#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_latch.h"
#include "core/ns_stdtypes.h"
#include "core/ns_testing.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"

#include <stdbool.h>
#include <stddef.h>

/******************************************************************************
 * SECTION: WAL Input Stream
 * ----------------------------------------------------------------------------
 * @brief Reads WAL entries from a WAL file
 *
 * curlsn is the committed LSN: the byte offset in the WAL file of the start
 * of the current log record being read, advanced by walis_mark_end_log().
 * lsnidx tracks bytes read within the current record (reset by
 * walis_mark_start_log()) so that curlsn is updated atomically per record.
 *
 * A partial read (fewer bytes than requested) means a torn record at the
 * end of the WAL; walis_read_all() treats this as soft EOF (iseof=true)
 * and seeks back to curlsn, leaving the file position at the last complete
 * record boundary.
 ******************************************************************************/

DEFINE_DBG_ASSERT (struct wal_istream, wal_istream, w, { ASSERT (w); })

struct wal_istream
{
  struct i_mem mem;
  struct i_os  os;     // The OS that opened [fd] - it must close it too
  i_file       fd;     // The file we're reading
  lsn          curlsn; // Where we are within the entire log file
  lsn          lsnidx; // Where we are within the current log

  latch        latch;
};

////////////////////////////////////////////////////////////
/// LOGR Mode

struct wal_istream *
walis_open (const char *fname, struct i_mem mem, struct i_os os, error *e)
{
  struct wal_istream *dest = mem.table->malloc (mem.self, 1, sizeof *dest, e);
  if (dest == NULL) {
    return NULL;
  }

  dest->mem = mem;
  dest->os  = os;

  /**
   * We'll open in write mode too
   * because if we reach the end on a corrupt
   * record, it truncates the file.
   *
   * In the future I forsee this going away.
   */
  if (os.table->open_file_r (os.self, &dest->fd, fname, e)) {
    mem.table->free (mem.self, dest);
    return NULL;
  }

  const i64 len = dest->fd.table->file_size (dest->fd.self, e);
  if (len < 0) {
    os.table->close_file (os.self, dest->fd.self, e);
    mem.table->free (mem.self, dest);
    return NULL;
  }

  if (dest->fd.table->seek (dest->fd.self, 0, I_SEEK_SET, e) < 0) {
    os.table->close_file (os.self, dest->fd.self, e);
    mem.table->free (mem.self, dest);
    return NULL;
  }

  dest->curlsn = 0;
  dest->lsnidx = 0;
  latch_init (&dest->latch);

  DBG_ASSERT (wal_istream, dest);

  return dest;
}

#ifndef NDEBUG
TEST (walis_open)
{
  TEST_CASE ("happy path")
  {
    ctx->os.table->remove_quiet (ctx->os.self, "foo", &ctx->e);
    i_file fp = {0};
    ctx->os.table->open_file_w (ctx->os.self, &fp, "foo", &ctx->e);
    ctx->os.table->close_file (ctx->os.self, fp.self, &ctx->e);

    struct wal_istream *wis = walis_open ("foo", ctx->mem, ctx->os, &ctx->e);
    test_assert (wis != NULL);
    walis_close (wis, &ctx->e);
    ctx->os.table->remove_quiet (ctx->os.self, "foo", &ctx->e);
  }
}
#endif

err_t
walis_close (struct wal_istream *w, error *e)
{
  DBG_ASSERT (wal_istream, w);
  w->os.table->close_file (w->os.self, w->fd.self, e);
  w->mem.table->free (w->mem.self, w);
  return error_trace (e);
}

err_t
walis_seek (struct wal_istream *w, const lsn pos, error *e)
{
  latch_lock (&w->latch);

  DBG_ASSERT (wal_istream, w);

  const i64 res = w->fd.table->seek (w->fd.self, pos, I_SEEK_SET, e);
  if (res < 0) {
    latch_unlock (&w->latch);
    return error_trace (e);
  }

  if ((u64)res != pos) {
    latch_unlock (&w->latch);
    return error_causef (e, ERR_CORRUPT, "seek to invalid offset");
  }

  w->curlsn = pos;

  latch_unlock (&w->latch);

  return SUCCESS;
}

err_t
walis_read_all (
    struct wal_istream *w,
    bool               *iseof,
    lsn                *rlsn,
    u32                *checksum,
    void               *data,
    const u32           len,
    error              *e
)
{
  latch_lock (&w->latch);

  DBG_ASSERT (wal_istream, w);

  *iseof = false;
  if (rlsn) {
    *rlsn = w->curlsn;
  }

  const i64 bread = w->fd.table->read_all (w->fd.self, data, len, e);
  if (bread < 0) {
    latch_unlock (&w->latch);
    return error_trace (e);
  }

  if (bread < len) {
    // Hit EOF - incomplete record (torn write at end of WAL)
    if ((bread > 0) && (walis_seek (w, w->curlsn, e)))
    // Partial read: seek back so the file position is at the
    // record start, leaving it at the last fully-written
    // record boundary
    {
      latch_unlock (&w->latch);
      return error_trace (e);
    }

    *iseof = true;
    latch_unlock (&w->latch);
    return SUCCESS;
  }

  if (checksum) {
    checksum_execute (checksum, data, len);
  }

  w->lsnidx += len;

  latch_unlock (&w->latch);

  return SUCCESS;
}

void
walis_mark_start_log (struct wal_istream *w)
{
  latch_lock (&w->latch);
  w->lsnidx = 0; // Reset intra-record byte counter before reading a new record
  latch_unlock (&w->latch);
}

void
walis_mark_end_log (struct wal_istream *w)
{
  latch_lock (&w->latch);
  w->curlsn += w->lsnidx; // Advance committed LSN by the bytes just consumed
  latch_unlock (&w->latch);
}

err_t
walis_crash (struct wal_istream *w, error *e)
{
  DBG_ASSERT (wal_istream, w);
  w->os.table->close_file (w->os.self, w->fd.self, e);
  w->mem.table->free (w->mem.self, w);
  return error_trace (e);
}

#ifndef NDEBUG

#  define WALIS_CRASH_T_FILE "walis_crash_t.wal"

/// Writes [len] bytes of [src] as the whole contents of the test wal
static void
walis_test_seed (struct test_ctx *ctx, const void *src, const u32 len)
{
  ctx->os.table->remove_quiet (ctx->os.self, WALIS_CRASH_T_FILE, &ctx->e);

  i_file f;
  test_assert_int_equal (
      ctx->os.table->open_file_w (ctx->os.self, &f, WALIS_CRASH_T_FILE, &ctx->e),
      SUCCESS
  );
  test_assert_int_equal (f.table->write_all (f.self, src, len, &ctx->e), SUCCESS);
  test_assert_int_equal (ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e), SUCCESS);
}

/// walis_crash abandons a reader mid-stream: it has to release the file
/// handle and the allocation (a leaked fd trips the OS teardown assert in
/// test_ctx_free) and, unlike the writer's crash, it must not be able to
/// damage the log it was reading.
TEST (walis_crash)
{
  const u8 payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

  TEST_CASE ("crashing part way through a record leaves the log alone")
  {
    walis_test_seed (ctx, payload, sizeof payload);

    struct wal_istream *wis = walis_open (WALIS_CRASH_T_FILE, ctx->mem, ctx->os, &ctx->e);
    test_assert (wis != NULL);

    // Read the first half of a record and die before marking its end, so the
    // committed lsn is still 0 while 4 bytes have been consumed
    walis_mark_start_log (wis);

    bool iseof    = true;
    lsn  rlsn     = 1;
    u32  checksum = 0;
    u8   buf[4]   = {0};

    test_assert_int_equal (
        walis_read_all (wis, &iseof, &rlsn, &checksum, buf, sizeof buf, &ctx->e),
        SUCCESS
    );
    test_assert (!iseof);
    test_assert_int_equal ((i32)rlsn, 0);
    test_assert_memequal (buf, payload, sizeof buf);

    test_assert_int_equal (walis_crash (wis, &ctx->e), SUCCESS);

    // The reader held the only handle and it opened read only, so every byte
    // is still there for the next reader
    i_file f;
    test_assert_int_equal (
        ctx->os.table->open_file_r (ctx->os.self, &f, WALIS_CRASH_T_FILE, &ctx->e),
        SUCCESS
    );
    test_assert_int_equal ((i32)f.table->file_size (f.self, &ctx->e), (i32)sizeof payload);
    ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e);

    ctx->os.table->remove_quiet (ctx->os.self, WALIS_CRASH_T_FILE, &ctx->e);
  }

  TEST_CASE ("a fresh reader starts over from the top")
  {
    walis_test_seed (ctx, payload, sizeof payload);

    struct wal_istream *wis = walis_open (WALIS_CRASH_T_FILE, ctx->mem, ctx->os, &ctx->e);
    test_assert (wis != NULL);

    bool iseof  = true;
    u8   buf[4] = {0};
    walis_mark_start_log (wis);
    test_assert_int_equal (
        walis_read_all (wis, &iseof, NULL, NULL, buf, sizeof buf, &ctx->e),
        SUCCESS
    );
    walis_mark_end_log (wis);
    test_assert_int_equal (walis_crash (wis, &ctx->e), SUCCESS);

    // No read position is persisted anywhere, so the replacement reader sees
    // the log from byte 0 again - the whole of it, in one read this time
    wis = walis_open (WALIS_CRASH_T_FILE, ctx->mem, ctx->os, &ctx->e);
    test_assert (wis != NULL);

    u8 all[8] = {0};
    walis_mark_start_log (wis);
    test_assert_int_equal (
        walis_read_all (wis, &iseof, NULL, NULL, all, sizeof all, &ctx->e),
        SUCCESS
    );
    test_assert (!iseof);
    test_assert_memequal (all, payload, sizeof payload);

    test_assert_int_equal (walis_close (wis, &ctx->e), SUCCESS);
    ctx->os.table->remove_quiet (ctx->os.self, WALIS_CRASH_T_FILE, &ctx->e);
  }

  TEST_CASE ("crashing straight after open")
  {
    walis_test_seed (ctx, payload, sizeof payload);

    struct wal_istream *wis = walis_open (WALIS_CRASH_T_FILE, ctx->mem, ctx->os, &ctx->e);
    test_assert (wis != NULL);
    test_assert_int_equal (walis_crash (wis, &ctx->e), SUCCESS);

    ctx->os.table->remove_quiet (ctx->os.self, WALIS_CRASH_T_FILE, &ctx->e);
  }
}

#endif // TESTING
