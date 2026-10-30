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

#include "nscore/wal/ns_wal_ostream.h"

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_numerics.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"

#ifndef NDEBUG
#  include "core/ns_testing.h"
#endif

#include <stddef.h>

/******************************************************************************
 * SECTION: WAL OStream
 * ----------------------------------------------------------------------------
 * @brief Output stream for a write ahead log
 *
 * Responsible for writing wal entries to a destination WAL file
 ******************************************************************************/

DEFINE_DBG_ASSERT (struct wal_ostream, wal_ostream, w, { ASSERT (w); })

struct wal_ostream *
walos_open (const char *fname, struct i_mem mem, struct i_os os, error *e)
{
  struct wal_ostream *ret = mem.table->malloc (mem.self, 1, sizeof *ret, e);
  if (ret == NULL) {
    return NULL;
  }

  ret->mem = mem;
  ret->os  = os;

  if (os.table->open_file_w (os.self, &ret->fd, fname, e)) {
    goto err_free;
  }

  const i64 len = ret->fd.table->seek (ret->fd.self, 0, I_SEEK_END, e);
  if (len < 0) {
    goto err_close;
  }

  latch_init (&ret->l);
  ret->buffer      = cbuffer_create (ret->_buffer, sizeof (ret->_buffer));
  ret->flushed_lsn = len;

  DBG_ASSERT (wal_ostream, ret);
  return ret;

err_close:
  os.table->close_file (os.self, ret->fd.self, e);
err_free:
  mem.table->free (mem.self, ret);
  return NULL;
}

#ifndef NDEBUG
TEST (walos_open)
{
  TEST_CASE ("happy path")
  {
    ctx->os.table->remove_quiet (ctx->os.self, "foo", &ctx->e);
    struct wal_ostream *wos = walos_open ("foo", ctx->mem, ctx->os, &ctx->e);
    test_assert (wos != NULL);
    walos_close (wos, &ctx->e);
    ctx->os.table->remove_quiet (ctx->os.self, "foo", &ctx->e);
  }
}
#endif

err_t
walos_close (struct wal_ostream *w, error *e)
{
  DBG_ASSERT (wal_ostream, w);
  walos_flush_all (w, e);
  w->os.table->close_file (w->os.self, w->fd.self, e);
  w->mem.table->free (w->mem.self, w);
  return error_trace (e);
}

err_t
walos_crash (struct wal_ostream *w, error *e)
{
  DBG_ASSERT (wal_ostream, w);
  w->os.table->close_file (w->os.self, w->fd.self, e);
  w->mem.table->free (w->mem.self, w);
  return error_trace (e);
}

#ifndef NDEBUG

#  define WALOS_CRASH_T_FILE "walos_crash_t.wal"

/// Bytes on disk for [fname]. The crash tests are only ever asking "did this
/// reach the file", and reopening is the only way to ask that from outside
/// the stream that was just thrown away.
static i64
walos_test_file_size (struct test_ctx *ctx, const char *fname)
{
  i_file f;
  if (ctx->os.table->open_file_r (ctx->os.self, &f, fname, &ctx->e) != SUCCESS) {
    return -1;
  }
  const i64 size = f.table->file_size (f.self, &ctx->e);
  ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e);
  return size;
}

/// walos_crash is walos_close minus the flush: it models the process dying
/// with a dirty WAL buffer, so everything that had not been fsynced has to be
/// lost and everything that had been has to survive. It still owns the file
/// handle and the allocation, so it still has to release both - a leaked fd
/// trips the OS teardown assert in test_ctx_free.
TEST (walos_crash)
{
  const u8 payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

  TEST_CASE ("unflushed writes never reach the file")
  {
    ctx->os.table->remove_quiet (ctx->os.self, WALOS_CRASH_T_FILE, &ctx->e);

    struct wal_ostream *wos = walos_open (WALOS_CRASH_T_FILE, ctx->mem, ctx->os, &ctx->e);
    test_assert (wos != NULL);

    test_assert_int_equal (walos_write_all (wos, NULL, payload, sizeof payload, &ctx->e), SUCCESS);

    // Buffered only: the next lsn moved, the flushed one did not
    test_assert_int_equal ((i32)walos_get_next_lsn (wos), (i32)sizeof payload);
    test_assert_int_equal ((i32)wos->flushed_lsn, 0);

    test_assert_int_equal (walos_crash (wos, &ctx->e), SUCCESS);

    test_assert_int_equal ((i32)walos_test_file_size (ctx, WALOS_CRASH_T_FILE), 0);

    ctx->os.table->remove_quiet (ctx->os.self, WALOS_CRASH_T_FILE, &ctx->e);
  }

  TEST_CASE ("flushed writes survive, later ones do not")
  {
    ctx->os.table->remove_quiet (ctx->os.self, WALOS_CRASH_T_FILE, &ctx->e);

    struct wal_ostream *wos = walos_open (WALOS_CRASH_T_FILE, ctx->mem, ctx->os, &ctx->e);
    test_assert (wos != NULL);

    test_assert_int_equal (walos_write_all (wos, NULL, payload, sizeof payload, &ctx->e), SUCCESS);
    test_assert_int_equal (walos_flush_all (wos, &ctx->e), SUCCESS);
    test_assert_int_equal ((i32)wos->flushed_lsn, (i32)sizeof payload);

    // A second write that is still sitting in the buffer when we die
    test_assert_int_equal (walos_write_all (wos, NULL, payload, sizeof payload, &ctx->e), SUCCESS);
    test_assert_int_equal ((i32)walos_get_next_lsn (wos), 2 * (i32)sizeof payload);

    test_assert_int_equal (walos_crash (wos, &ctx->e), SUCCESS);

    // Exactly the flushed prefix is on disk
    test_assert_int_equal (
        (i32)walos_test_file_size (ctx, WALOS_CRASH_T_FILE),
        (i32)sizeof payload
    );

    i_file f;
    test_assert_int_equal (
        ctx->os.table->open_file_r (ctx->os.self, &f, WALOS_CRASH_T_FILE, &ctx->e),
        SUCCESS
    );
    u8 readback[8] = {0};
    test_assert_int_equal (
        (i32)f.table->read_all (f.self, readback, sizeof readback, &ctx->e),
        (i32)sizeof readback
    );
    test_assert_memequal (readback, payload, sizeof payload);
    ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e);

    ctx->os.table->remove_quiet (ctx->os.self, WALOS_CRASH_T_FILE, &ctx->e);
  }

  TEST_CASE ("crashing straight after open leaves an empty wal behind")
  {
    ctx->os.table->remove_quiet (ctx->os.self, WALOS_CRASH_T_FILE, &ctx->e);

    struct wal_ostream *wos = walos_open (WALOS_CRASH_T_FILE, ctx->mem, ctx->os, &ctx->e);
    test_assert (wos != NULL);
    test_assert_int_equal (walos_crash (wos, &ctx->e), SUCCESS);

    // walos_open created the file, so it exists and is empty - not missing
    test_assert_int_equal ((i32)walos_test_file_size (ctx, WALOS_CRASH_T_FILE), 0);

    ctx->os.table->remove_quiet (ctx->os.self, WALOS_CRASH_T_FILE, &ctx->e);
  }
}

#endif // TESTING

static err_t
walos_flush_impl (struct wal_ostream *w, error *e)
{
  const u32 towrite = cbuffer_len (&w->buffer);
  if (towrite == 0) {
    return SUCCESS;
  }

  if (cbuffer_write_to_file_1_expect (&w->fd, &w->buffer, towrite, e)) {
    panic ("Wal write failed");
  }
  cbuffer_write_to_file_2 (&w->buffer, towrite);

  if (w->fd.table->fsync (w->fd.self, e)) {
    panic ("Wal fsync failed");
  }

  w->flushed_lsn += towrite;
  return SUCCESS;
}

err_t
walos_flush_all (struct wal_ostream *w, error *e)
{
  DBG_ASSERT (wal_ostream, w);
  latch_lock (&w->l);
  const err_t ret = walos_flush_impl (w, e);
  latch_unlock (&w->l);
  return ret;
}

err_t
walos_write_all (struct wal_ostream *w, u32 *checksum, const void *data, const u32 len, error *e)
{
  DBG_ASSERT (wal_ostream, w);

  if (checksum) {
    checksum_execute (checksum, data, len);
  }

  u32       written = 0;
  const u8 *src     = data;

  latch_lock (&w->l);

  while (written < len) {
    if ((cbuffer_avail (&w->buffer) == 0) && (walos_flush_impl (w, e))) {
      latch_unlock (&w->l);
      return error_trace (e);
    }

    const u32 towrite = MIN (len - written, cbuffer_avail (&w->buffer));
    if (towrite > 0) {
      cbuffer_write_expect (src + written, 1, towrite, &w->buffer);
      written += towrite;
    }
  }

  latch_unlock (&w->l);
  return SUCCESS;
}

lsn
walos_get_next_lsn (struct wal_ostream *w)
{
  latch_lock (&w->l);
  lsn ret = w->flushed_lsn + cbuffer_len (&w->buffer);
  latch_unlock (&w->l);
  return ret;
}

slsn
walos_truncate (struct wal_ostream *w, error *e)
{
  latch_lock (&w->l);
  if (w->fd.table->truncate (w->fd.self, 0, e)) {
    goto theend;
  }
  if (w->fd.table->seek (w->fd.self, 0, I_SEEK_SET, e)) {
    goto theend;
  }

theend:
  latch_unlock (&w->l);
  return error_trace (e);
}
