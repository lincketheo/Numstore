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

#include "nscore/disk_pager/ns_file_pager.h"

#include "core/ns_csx_assert.h"
#include "core/ns_error.h"
#include "core/ns_latch.h"
#include "core/ns_testing.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"

#include <stdatomic.h>
#include <string.h>

// pgno ...etc

enum file_pager_flags
{
  FP_ISNEW = (1 << 0),
};

struct file_pager
{
  struct i_mem mem;
  struct i_os  os; // The OS that opened [f] - it must close it too
  _Atomic pgno npages;
  i_file       f;
  u32          header_len;
  int          flags;
  latch        l;
};

DEFINE_DBG_ASSERT (struct file_pager, file_pager, p, { ASSERT (p); })

struct file_pager *
fpgr_open (const char *dbname, struct i_mem mem, struct i_os os, u32 header_len, error *e)
{
  // Allocate space for the pager
  struct file_pager *dest = mem.table->malloc (mem.self, 1, sizeof *dest, e);
  if (dest == NULL) {
    return NULL;
  }

  // Basic initialization
  dest->mem = mem;
  dest->os  = os;
  latch_init (&dest->l);
  dest->flags = 0;

  // Open the database in read write mode
  if (os.table->open_file_rw (os.self, &dest->f, dbname, e)) {
    goto failed;
  }

  // We'll use the size for checking if it's new or not
  i64 size = dest->f.table->file_size (dest->f.self, e);

  // Failed
  if (size < 0) {
    goto fp_failed;
  }

  // No header or a newly created file (just a header)
  if (size == 0 || size == header_len) {
    dest->flags |= FP_ISNEW;

    // extend the file to the header length
    if (size == 0) {
      if (dest->f.table->truncate (dest->f.self, header_len, e)) {
        goto fp_failed;
      }
    }

    size = header_len;
  }

  // This file has a header that is less than the length of the file
  else if (size < header_len) {
    error_causef (e, ERR_CORRUPT, "Invalid file pager header\n");
    goto fp_failed;
  }

  // Corrupt database - not a multiple of NS_PAGE_SIZE
  if ((size - header_len) % NS_PAGE_SIZE != 0) {
    error_causef (e, ERR_CORRUPT, "File pager does not contain contiguous pagers\n");
    goto fp_failed;
  }

  atomic_store (&dest->npages, (size - header_len) / NS_PAGE_SIZE);
  dest->header_len = header_len;

  DBG_ASSERT (file_pager, dest);

  return dest;

fp_failed:
  os.table->close_file (os.self, dest->f.self, e);

// Reached before the file is open, so there is nothing to close -- but [dest]
// is already allocated either way.
failed:
  mem.table->free (mem.self, dest);
  return NULL;
}

#ifndef NDEBUG
TEST (fpgr_open)
{
  _Static_assert (NS_PAGE_SIZE > 2, "NS_PAGE_SIZE should be > 2 for file_pager test");

  i_file fp = {0};
  ctx->os.table->open_file_rw (ctx->os.self, &fp, "test.db", &ctx->e);

  // edge case: file shorter than header
  test_fail_if (fp.table->truncate (fp.self, NS_PAGE_SIZE - 1, &ctx->e));
  struct file_pager *pager = fpgr_open ("test.db", ctx->mem, ctx->os, 0, &ctx->e);
  test_err_t_check (ctx->e.cause_code, ERR_CORRUPT, &ctx->e);

  // edge case: file size = half a page
  test_fail_if (fp.table->truncate (fp.self, NS_PAGE_SIZE / 2, &ctx->e));
  pager = fpgr_open ("test.db", ctx->mem, ctx->os, 0, &ctx->e);
  test_err_t_check (ctx->e.cause_code, ERR_CORRUPT, &ctx->e);

  // happy path: file exactly header size, zero pages
  test_fail_if (fp.table->truncate (fp.self, 0, &ctx->e));
  pager = fpgr_open ("test.db", ctx->mem, ctx->os, 0, &ctx->e);
  test_assert_int_equal ((int)atomic_load (&pager->npages), 0);
  test_fail_if (fpgr_close (pager, &ctx->e));

  // happy path: file exactly header size, more pages
  test_fail_if (fp.table->truncate (fp.self, 3 * NS_PAGE_SIZE, &ctx->e));
  pager = fpgr_open ("test.db", ctx->mem, ctx->os, 0, &ctx->e);
  test_assert_equal (atomic_load (&pager->npages), 3);
  test_fail_if (fpgr_close (pager, &ctx->e));

  // There were 2 refs to file - close it here too
  test_fail_if (ctx->os.table->close_file (ctx->os.self, fp.self, &ctx->e));
  test_fail_if (ctx->os.table->unlink (ctx->os.self, "test.db", &ctx->e));
}
#endif

err_t
fpgr_close (struct file_pager *f, error *e)
{
  DBG_ASSERT (file_pager, f);
  latch_lock (&f->l); // Never release this
  f->os.table->close_file (f->os.self, f->f.self, e);
  f->mem.table->free (f->mem.self, f);
  return error_trace (e);
}

err_t
fpgr_reset (struct file_pager *f, error *e)
{
  DBG_ASSERT (file_pager, f);

  latch_lock (&f->l);
  if (f->f.table->truncate (f->f.self, f->header_len, e)) {
    latch_unlock (&f->l);
    return error_trace (e);
  }
  atomic_store (&f->npages, 0);
  latch_unlock (&f->l);

  return error_trace (e);
}

bool
fpgr_isnew (struct file_pager *f)
{
  return (f->flags & FP_ISNEW) != 0;
}

p_size
fpgr_get_npages (const struct file_pager *fp)
{
  DBG_ASSERT (file_pager, fp);
  return atomic_load (&fp->npages);
}

err_t
fpgr_extend (struct file_pager *p, pgno dest, error *e)
{
  DBG_ASSERT (file_pager, p);
  ASSERT (dest);

  latch_lock (&p->l);

  if (dest < atomic_load (&p->npages)) {
    latch_unlock (&p->l);
    return SUCCESS;
  }

  if (p->f.table->truncate (p->f.self, p->header_len + NS_PAGE_SIZE * (dest), e)) {
    latch_unlock (&p->l);
    goto failed;
  }

  atomic_store (&p->npages, dest);
  latch_unlock (&p->l);

  return SUCCESS;

failed:
  return error_trace (e);
}

#ifndef NDEBUG
TEST (fpgr_new)
{
  i_file fp = {0};

  test_fail_if (ctx->os.table->open_file_rw (ctx->os.self, &fp, "test.db", &ctx->e));

  test_fail_if (fp.table->truncate (fp.self, 0, &ctx->e));

  struct file_pager *pager = fpgr_open ("test.db", ctx->mem, ctx->os, 0, &ctx->e);

  // Create a new page
  test_fail_if (fpgr_extend (pager, 1, &ctx->e));
  test_assert_int_equal (atomic_load (&pager->npages), 1);
  test_assert_int_equal (
      fp.table->file_size (fp.self, &ctx->e),
      NS_PAGE_SIZE * atomic_load (&pager->npages)
  );

  // Add two more pages and do the same thing
  test_fail_if (fpgr_extend (pager, 2, &ctx->e));
  test_assert_int_equal (atomic_load (&pager->npages), 2);
  test_assert_int_equal (
      fp.table->file_size (fp.self, &ctx->e),
      NS_PAGE_SIZE * atomic_load (&pager->npages)
  );

  test_fail_if (fpgr_extend (pager, 3, &ctx->e));
  test_assert_int_equal (atomic_load (&pager->npages), 3);
  test_assert_int_equal (
      fp.table->file_size (fp.self, &ctx->e),
      NS_PAGE_SIZE * atomic_load (&pager->npages)
  );

  test_fail_if (fpgr_close (pager, &ctx->e));

  // There were 2 refs to file - close it here too
  test_fail_if (ctx->os.table->close_file (ctx->os.self, fp.self, &ctx->e));
  test_fail_if (ctx->os.table->unlink (ctx->os.self, "test.db", &ctx->e));
}
#endif

err_t
fpgr_read (struct file_pager *p, u8 *dest, pgno pg, error *e)
{
  ASSERT (dest);

  latch_lock (&p->l);

  DBG_ASSERT (file_pager, p);

  if (pg >= atomic_load (&p->npages)) {
    error_causef (
        e,
        ERR_PG_OUT_OF_RANGE,
        "page %" PRpgno " out of range (npages=%" PRpgno ")",
        pg,
        atomic_load (&p->npages)
    );
    goto theend;
  }

  // Read all from file
  const i64 nread = p->f.table->pread_all (
      p->f.self,
      dest,
      NS_PAGE_SIZE,
      p->header_len + pg * NS_PAGE_SIZE,
      e
  );

  if (nread == 0) {
    error_causef (e, ERR_CORRUPT, "pread returned 0 bytes at page %" PRpgno, pg);
    goto theend;
  }

  if (nread < 0) {
    goto theend;
  }

theend:

  latch_unlock (&p->l);

  return error_trace (e);
}

err_t
fpgr_write (struct file_pager *p, const u8 *src, const pgno pg, error *e)
{
  ASSERT (src);

  latch_lock (&p->l);

  DBG_ASSERT (file_pager, p);
  ASSERT (pg < atomic_load (&p->npages));

  if (p->f.table->pwrite_all (p->f.self, src, NS_PAGE_SIZE, p->header_len + pg * NS_PAGE_SIZE, e)) {
    goto theend;
  }

theend:

  latch_unlock (&p->l);

  return error_trace (e);
}

err_t
fpgr_write_header (struct file_pager *p, const u8 *src, u32 ofst, u32 size, error *e)
{
  ASSERT (src);
  ASSERT (ofst + size <= p->header_len);

  latch_lock (&p->l);

  DBG_ASSERT (file_pager, p);

  if (p->f.table->pwrite_all (p->f.self, src, size, ofst, e)) {
    goto theend;
  }

  if (p->f.table->fsync (p->f.self, e)) {
    goto theend;
  }

theend:

  latch_unlock (&p->l);

  return error_trace (e);
}

err_t
fpgr_read_header (struct file_pager *p, u8 *dest, u32 ofst, u32 size, error *e)
{
  ASSERT (dest);
  ASSERT (ofst + size <= p->header_len);

  latch_lock (&p->l);

  DBG_ASSERT (file_pager, p);

  // Read all from file
  if (file_pread_all_expect (p->f, dest, size, ofst, e)) {
    goto theend;
  }

theend:

  latch_unlock (&p->l);

  return error_trace (e);
}

#ifndef NDEBUG
TEST (fpgr_read_write)
{
  // The raw page bytes
  u8     _page[NS_PAGE_SIZE];

  // Create a temporary file
  i_file fp = {0};

  test_fail_if (ctx->os.table->open_file_rw (ctx->os.self, &fp, "test.db", &ctx->e));

  // File should be size 0
  test_fail_if (fp.table->truncate (fp.self, 0, &ctx->e));

  // Open a new pager
  struct file_pager *pager = fpgr_open ("test.db", ctx->mem, ctx->os, 0, &ctx->e);
  test_assert_int_equal (ctx->e.cause_code, SUCCESS);
  // happy path: new page, write, then read back
  test_fail_if (fpgr_extend (pager, 2, &ctx->e));

  // Write 0 : NS_PAGE_SIZE to each byte (overflow fine, it's just data)
  for (u32 i = 0; i < NS_PAGE_SIZE; i++) {
    _page[i] = (u8)i;
  }
  // Write it out
  test_fail_if (fpgr_write (pager, _page, 1, &ctx->e));

  // Scramble page so we can read it back in
  memset (_page, 0xFF, NS_PAGE_SIZE);
  test_fail_if (fpgr_read (pager, _page, 1, &ctx->e));

  // Iterate and check that it matches what we expect
  for (u32 i = 0; i < NS_PAGE_SIZE; i++) {
    test_assert_int_equal (_page[i], (u8)i);
  }

  // There's 2 refs to this file, close the other one
  test_fail_if (fpgr_close (pager, &ctx->e));
  test_fail_if (ctx->os.table->close_file (ctx->os.self, fp.self, &ctx->e));
  test_fail_if (ctx->os.table->unlink (ctx->os.self, "test.db", &ctx->e));
}
#endif

err_t
fpgr_crash (struct file_pager *p, error *e)
{
  DBG_ASSERT (file_pager, p);
  latch_lock (&p->l);
  p->os.table->close_file (p->os.self, p->f.self, e);
  p->mem.table->free (p->mem.self, p);
  return error_trace (e);
}
