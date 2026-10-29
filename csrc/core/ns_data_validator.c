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

#include "core/ns_data_validator.h"

#include "core/ns_numerics.h"
#include "core/ns_stride.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"

#ifndef NDEBUG
#  include "core/ns_ext_array.h" // ext_array - the writer the tests drive
#  include "core/ns_testing.h"   // TEST
#endif

#include <inttypes.h>
#include <string.h>

/******************************************************************************
 * SECTION: Data Validator
 ******************************************************************************/

static err_t
dvalidtr_light_validate (const struct dvalidtr *d, error *e)
{
  const i64 sut_len = d->sut.functions.getlen (d->sut.ctx, e);
  if (sut_len < 0) {
    return error_trace (e);
  }

  const i64 ref_len = d->ref.functions.getlen (d->ref.ctx, e);
  if (ref_len < 0) {
    return error_trace (e);
  }

  if (sut_len != ref_len) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "length mismatch: ref=%" PRIu64 " sut=%" PRIu64 " (delta=%" PRIu64 ")",
        ref_len,
        sut_len,
        (i64)sut_len - (i64)ref_len
    );
  }

  return SUCCESS;
}

static err_t
dvalidtr_read (
    const struct dvalidtr *d,
    const struct stride    str,
    const u32              size,
    void                  *_dest,
    error                 *e
)
{
  void *ref  = d->mem.table->malloc (d->mem.self, str.nelems, size, e);
  void *dest = _dest;

  if (ref == NULL) {
    goto theend;
  }

  if (_dest == NULL) {
    dest = d->mem.table->malloc (d->mem.self, str.nelems, size, e);
    if (dest == NULL) {
      goto theend;
    }
  }

  // Read from the ref
  const i64 ref_read = d->ref.functions.read (d->ref.ctx, str, size, ref, e);
  if (ref_read < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "ref read failed (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  // Read from the system under test
  const i64 sut_read = d->sut.functions.read (d->sut.ctx, str, size, dest, e);
  if (sut_read < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "sut read failed (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  if (ref_read != sut_read) {
    error_causef (
        e,
        ERR_CORRUPT,
        "read count mismatch: ref=%" PRIu64 " sut=%" PRIu64 " (start=%" PRIu64 " stride=%" PRIu64
        " nelems=%" PRIu64 " size=%u)",
        ref_read,
        sut_read,
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  if (memcmp (dest, ref, (u64)sut_read * size) != 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "read data mismatch: ref/sut diverged"
        " (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u nread=%" PRIu64 ")",
        str.start,
        str.stride,
        str.nelems,
        size,
        sut_read
    );
    goto theend;
  }

theend:
  if (ref) {
    d->mem.table->free (d->mem.self, ref);
  }
  if (_dest == NULL && dest) {
    d->mem.table->free (d->mem.self, dest);
  }
  return error_trace (e);
}

static err_t
dvalidtr_insert (struct dvalidtr *d, const u32 ofst, const void *_src, const u32 slen, error *e)
{
  u8 *src = (u8 *)_src;
  if (_src == NULL) {
    src = d->mem.table->malloc (d->mem.self, slen, 1, e);
    if (src == NULL) {
      goto theend;
    }
    ptr_range (src, slen);
  }

  // Insert into ref
  const i64 ref_written = d->ref.functions.insert (d->ref.ctx, ofst, src, slen, e);
  if (ref_written < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "insert into ref failed at byte offset "
        "%u (slen=%u)",
        ofst,
        slen
    );
    goto theend;
  }

  // Insert into system under test
  const i64 sut_written = d->sut.functions.insert (d->sut.ctx, ofst, src, slen, e);
  if (sut_written < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "insert into sut failed at byte offset "
        "%u (slen=%u)",
        ofst,
        slen
    );
    goto theend;
  }

  if (ref_written != sut_written) {
    error_causef (
        e,
        ERR_CORRUPT,
        "insert count mismatch: ref=%" PRIu64 " sut=%" PRIu64,
        ref_written,
        sut_written
    );
    goto theend;
  }

  // Read back what we just inserted (byte-level, so size=1 and
  // nelems=slen)
  err_t sut_read = dvalidtr_read (
      d,
      (struct stride){
          .start  = ofst,
          .nelems = slen,
          .stride = 1,
      },
      1,
      NULL,
      e
  );
  if (sut_read < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "read-back after insert failed (byte "
        "offset=%u, slen=%u)",
        ofst,
        slen
    );
    goto theend;
  }

  if (dvalidtr_light_validate (d, e)) {
    error_causef (
        e,
        ERR_CORRUPT,
        "length check failed after insert at "
        "byte offset %u (slen=%u)",
        ofst,
        slen
    );
    goto theend;
  }

  if ((d->isvalid) && (d->isvalid (d->sut.ctx, e))) {
    goto theend;
  }

theend:
  if (_src == NULL) {
    d->mem.table->free (d->mem.self, src);
  }
  return error_trace (e);
}

static err_t
dvalidtr_write (
    struct dvalidtr    *d,
    const struct stride str,
    const u32           size,
    const void         *_src,
    error              *e
)
{
  u8 *src = (u8 *)_src;
  if (_src == NULL) {
    src = d->mem.table->malloc (d->mem.self, str.nelems, size, e);
    if (src == NULL) {
      goto theend;
    }
    ptr_range (src, str.nelems * size);
  }

  const i64 sut_written = d->sut.functions.write (d->sut.ctx, str, size, src, e);
  if (sut_written < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "write into sut failed (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  const i64 ref_written = d->ref.functions.write (d->ref.ctx, str, size, src, e);
  if (ref_written < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "write into ref failed (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  if (ref_written != sut_written) {
    error_causef (
        e,
        ERR_CORRUPT,
        "write count mismatch: ref=%" PRIu64 " sut=%" PRIu64 " (start=%" PRIu64 " stride=%" PRIu64
        " nelems=%" PRIu64 " size=%u)",
        ref_written,
        sut_written,
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  // Read back what we just wrote
  if (dvalidtr_read (d, str, size, NULL, e)) {
    error_causef (
        e,
        ERR_CORRUPT,
        "read-back after write failed (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64
        " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  if ((d->isvalid) && (d->isvalid (d->sut.ctx, e))) {
    goto theend;
  }

theend:
  if (_src == NULL && src) {
    d->mem.table->free (d->mem.self, src);
  }
  return error_trace (e);
}

static err_t
dvalidtr_remove (struct dvalidtr *d, const struct stride str, const u32 size, void *_dest, error *e)
{
  void *ref  = d->mem.table->malloc (d->mem.self, str.nelems, size, e);
  void *dest = _dest;

  if (ref == NULL) {
    goto theend;
  }

  if (_dest == NULL) {
    dest = d->mem.table->malloc (d->mem.self, str.nelems, size, e);
    if (dest == NULL) {
      goto theend;
    }
  }

  // Remove from the ref
  const i64 ref_read = d->ref.functions.remove (d->ref.ctx, str, size, ref, e);
  if (ref_read < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "remove from ref failed (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  // Remove from the system under test
  const i64 sut_read = d->sut.functions.remove (d->sut.ctx, str, size, dest, e);
  if (sut_read < 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "remove from sut failed (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  if (ref_read != sut_read) {
    error_causef (
        e,
        ERR_CORRUPT,
        "remove count mismatch: ref=%" PRIu64 " sut=%" PRIu64 " (start=%" PRIu64 " stride=%" PRIu64
        " nelems=%" PRIu64 " size=%u)",
        ref_read,
        sut_read,
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  if (memcmp (dest, ref, (u64)sut_read * size) != 0) {
    error_causef (
        e,
        ERR_CORRUPT,
        "remove data mismatch: ref/sut diverged"
        " (start=%" PRIu64 " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u nremoved=%" PRIu64 ")",
        str.start,
        str.stride,
        str.nelems,
        size,
        sut_read
    );
    goto theend;
  }

  if (dvalidtr_light_validate (d, e)) {
    error_causef (
        e,
        ERR_CORRUPT,
        "length check failed after remove "
        "(start=%" PRIu64 "stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
        str.start,
        str.stride,
        str.nelems,
        size
    );
    goto theend;
  }

  if ((d->isvalid) && (d->isvalid (d->sut.ctx, e))) {
    goto theend;
  }

theend:
  if (ref) {
    d->mem.table->free (d->mem.self, ref);
  }
  if (_dest == NULL && dest) {
    d->mem.table->free (d->mem.self, dest);
  }
  return error_trace (e);
}

static err_t
dvalidtr_getlen (const struct dvalidtr *d, error *e)
{
  return d->ref.functions.getlen (d->ref.ctx, e);
}

static err_t
dvalidtr_validate (struct dvalidtr *d, error *e)
{
  if (dvalidtr_light_validate (d, e)) {
    return error_trace (e);
  }

  const i64 len = d->ref.functions.getlen (d->ref.ctx, e);
  if (len < 0) {
    return error_trace (e);
  }

  if (dvalidtr_read (
          d,
          (struct stride){
              .start  = 0,
              .stride = 1,
              .nelems = len,
          },
          1,
          NULL,
          e
      )) {
    error_causef (e, ERR_CORRUPT, "full read validation failed (len=%" PRIu64 ")", len);
    return error_trace (e);
  }

  return SUCCESS;
}

err_t
dvalidtr_random_test (
    struct dvalidtr *d,
    const u32        size,
    const u32        niters,
    const u64        max_insert,
    error           *e
)
{
  for (u32 k = 0; k < niters; ++k) {
    i64 len = dvalidtr_getlen (d, e);
    if (len < 0) {
      return error_trace (e);
    }
    len /= size;

    enum
    {
      C_INSERT,
      C_READ,
      C_REMOVE,
      C_WRITE,
    } choice = randu32r (0, 3);

    if (len == 0) {
      choice = C_INSERT;
    }

    const u64           start   = len > 0 ? randu32r (0, (u32)len - 1) : 0;
    const u64           stride  = randu32r (1, 8);
    const u32           max_n   = len > 0 ? (((u32)len - 1 - start) / stride) + 1 : 0;
    const u64           nelems  = max_n > 0 ? randu32r (1, MIN (max_n, max_insert)) : 0;
    const u32           ninsert = randu32r (1, (u32)max_insert);

    const struct stride str     = {
        .start  = start,
        .stride = stride,
        .nelems = nelems,
    };

    switch (choice) {
      case C_INSERT: {
        if (dvalidtr_insert (d, start * size, NULL, ninsert * size, e)) {
          return error_causef (
              e,
              ERR_CORRUPT,
              "random test failed on iter %u: insert (len="
              "%" PRIu64 " start=%" PRIu64 " ninsert=%u size=%u)",
              k,
              len,
              start,
              ninsert,
              size
          );
        }
        break;
      }
      case C_READ: {
        if (dvalidtr_read (d, str, size, NULL, e)) {
          return error_causef (
              e,
              ERR_CORRUPT,
              "random test failed on iter %u: read (len=%" PRIu64 " start=%" PRIu64
              " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
              k,
              len,
              start,
              stride,
              nelems,
              size
          );
        }
        break;
      }
      case C_REMOVE: {
        if (dvalidtr_remove (d, str, size, NULL, e)) {
          return error_causef (
              e,
              ERR_CORRUPT,
              "random test failed on iter %u: remove (len=%" PRIu64 " start=%" PRIu64
              " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
              k,
              len,
              start,
              stride,
              nelems,
              size
          );
        }
        break;
      }
      case C_WRITE: {
        if (dvalidtr_write (d, str, size, NULL, e)) {
          return error_causef (
              e,
              ERR_CORRUPT,
              "random test failed on iter %u: write (len=%" PRIu64 " start=%" PRIu64
              " stride=%" PRIu64 " nelems=%" PRIu64 " size=%u)",
              k,
              len,
              start,
              stride,
              nelems,
              size
          );
        }
        break;
      }
    }
  }

  if (dvalidtr_validate (d, e)) {
    return error_causef (e, ERR_CORRUPT, "random test failed to validate data at the end");
  }

  return SUCCESS;
}

#ifndef NDEBUG

static struct dvalidtr
dv_create (struct ext_array *ref, struct ext_array *sut, const struct i_mem mem)
{
  struct dvalidtr d = {
      .mem     = mem,
      .isvalid = NULL,
  };
  ext_array_data_writer (&d.ref, ref);
  ext_array_data_writer (&d.sut, sut);
  return d;
}

static const u8 dv_payload[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};

TEST (dvalidtr_insert_agreeing_writers)
{
  struct ext_array ref = ext_array_create (ctx->mem);
  struct ext_array sut = ext_array_create (ctx->mem);
  struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);

  TEST_CASE ("Insert propagates to both writers")
  {
    test_assert_int_equal (
        dvalidtr_insert (&d, 0, dv_payload, sizeof dv_payload, &ctx->e),
        SUCCESS
    );
    test_assert_type_equal (ext_array_get_len (&ref), (u64)sizeof dv_payload, u64, PRIu64);
    test_assert_type_equal (ext_array_get_len (&sut), (u64)sizeof dv_payload, u64, PRIu64);
    test_assert_memequal (ref.data, sut.data, sizeof dv_payload);
  }

  TEST_CASE ("A second insert in the middle still agrees")
  {
    test_assert_int_equal (dvalidtr_insert (&d, 4, dv_payload, 4, &ctx->e), SUCCESS);
    test_assert_type_equal (ext_array_get_len (&sut), (u64)12, u64, PRIu64);
  }

  TEST_CASE ("getlen reports the ref length")
  {
    test_assert_int_equal (dvalidtr_getlen (&d, &ctx->e), 12);
  }

  TEST_CASE ("Full validation passes while the writers agree")
  {
    test_assert_int_equal (dvalidtr_light_validate (&d, &ctx->e), SUCCESS);
    test_assert_int_equal (dvalidtr_validate (&d, &ctx->e), SUCCESS);
  }

  ext_array_free (&ref);
  ext_array_free (&sut);
}

// NULL [_src] asks the validator to allocate and fill the payload itself, which
// is the path dvalidtr_random_test takes
TEST (dvalidtr_insert_generates_its_own_payload)
{
  struct ext_array ref = ext_array_create (ctx->mem);
  struct ext_array sut = ext_array_create (ctx->mem);
  struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);

  test_assert_int_equal (dvalidtr_insert (&d, 0, NULL, 16, &ctx->e), SUCCESS);
  test_assert_type_equal (ext_array_get_len (&sut), (u64)16, u64, PRIu64);
  test_assert_memequal (ref.data, sut.data, 16);

  ext_array_free (&ref);
  ext_array_free (&sut);
}

TEST (dvalidtr_read_write_remove_agreeing_writers)
{
  struct ext_array    ref = ext_array_create (ctx->mem);
  struct ext_array    sut = ext_array_create (ctx->mem);
  struct dvalidtr     d   = dv_create (&ref, &sut, ctx->mem);

  const struct stride all = {.start = 0, .stride = 1, .nelems = sizeof dv_payload};

  test_assert_int_equal (dvalidtr_insert (&d, 0, dv_payload, sizeof dv_payload, &ctx->e), SUCCESS);

  TEST_CASE ("Contiguous read agrees")
  {
    test_assert_int_equal (dvalidtr_read (&d, all, 1, NULL, &ctx->e), SUCCESS);
  }

  TEST_CASE ("Strided read agrees")
  {
    const struct stride every_other = {.start = 0, .stride = 2, .nelems = 4};
    test_assert_int_equal (dvalidtr_read (&d, every_other, 1, NULL, &ctx->e), SUCCESS);
  }

  TEST_CASE ("Read into a caller supplied buffer returns the ref bytes")
  {
    u8 dest[sizeof dv_payload] = {0};
    test_assert_int_equal (dvalidtr_read (&d, all, 1, dest, &ctx->e), SUCCESS);
    test_assert_memequal (dest, dv_payload, sizeof dv_payload);
  }

  TEST_CASE ("Write agrees and lands in both writers")
  {
    const u8            replacement[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    const struct stride head           = {.start = 0, .stride = 1, .nelems = 4};
    test_assert_int_equal (dvalidtr_write (&d, head, 1, replacement, &ctx->e), SUCCESS);
    test_assert_memequal (ref.data, replacement, 4);
    test_assert_memequal (sut.data, replacement, 4);
  }

  TEST_CASE ("Remove agrees and shortens both writers")
  {
    const struct stride head = {.start = 0, .stride = 1, .nelems = 4};
    test_assert_int_equal (dvalidtr_remove (&d, head, 1, NULL, &ctx->e), SUCCESS);
    test_assert_type_equal (ext_array_get_len (&ref), (u64)4, u64, PRIu64);
    test_assert_type_equal (ext_array_get_len (&sut), (u64)4, u64, PRIu64);
    test_assert_int_equal (dvalidtr_validate (&d, &ctx->e), SUCCESS);
  }

  ext_array_free (&ref);
  ext_array_free (&sut);
}

TEST (dvalidtr_detects_length_mismatch)
{
  struct ext_array ref = ext_array_create (ctx->mem);
  struct ext_array sut = ext_array_create (ctx->mem);
  struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);

  test_assert_int_equal (dvalidtr_insert (&d, 0, dv_payload, sizeof dv_payload, &ctx->e), SUCCESS);

  ext_array_insert (&sut, 0, dv_payload, 2, &ctx->e);
  test_assert_int_equal (ctx->e.cause_code, SUCCESS);

  TEST_CASE ("Light validation reports the mismatch")
  {
    test_err_t_check (dvalidtr_light_validate (&d, &ctx->e), ERR_CORRUPT, &ctx->e);
  }

  TEST_CASE ("Full validation reports the mismatch")
  {
    test_err_t_check (dvalidtr_validate (&d, &ctx->e), ERR_CORRUPT, &ctx->e);
  }

  ext_array_free (&ref);
  ext_array_free (&sut);
}

TEST (dvalidtr_detects_data_divergence)
{
  struct ext_array ref = ext_array_create (ctx->mem);
  struct ext_array sut = ext_array_create (ctx->mem);
  struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);

  test_assert_int_equal (dvalidtr_insert (&d, 0, dv_payload, sizeof dv_payload, &ctx->e), SUCCESS);

  sut.data[3] ^= 0xFF;

  TEST_CASE ("Lengths still agree, so light validation passes")
  {
    test_assert_int_equal (dvalidtr_light_validate (&d, &ctx->e), SUCCESS);
  }

  TEST_CASE ("A read covering the bad byte reports divergence")
  {
    const struct stride all = {.start = 0, .stride = 1, .nelems = sizeof dv_payload};
    test_err_t_check (dvalidtr_read (&d, all, 1, NULL, &ctx->e), ERR_CORRUPT, &ctx->e);
  }

  TEST_CASE ("A read that skips the bad byte still agrees")
  {
    const struct stride tail = {.start = 4, .stride = 1, .nelems = 4};
    test_assert_int_equal (dvalidtr_read (&d, tail, 1, NULL, &ctx->e), SUCCESS);
  }

  TEST_CASE ("Full validation reads everything and reports divergence")
  {
    test_err_t_check (dvalidtr_validate (&d, &ctx->e), ERR_CORRUPT, &ctx->e);
  }

  ext_array_free (&ref);
  ext_array_free (&sut);
}

static err_t
dv_isvalid_always_fails (void *vctx, error *e)
{
  (void)vctx;
  return error_causef (e, ERR_CORRUPT, "sut rejected by isvalid hook");
}

static err_t
dv_isvalid_always_passes (void *vctx, error *e)
{
  (void)vctx;
  (void)e;
  return SUCCESS;
}

TEST (dvalidtr_isvalid_hook)
{
  TEST_CASE ("A passing hook leaves insert succeeding")
  {
    struct ext_array ref = ext_array_create (ctx->mem);
    struct ext_array sut = ext_array_create (ctx->mem);
    struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);
    d.isvalid            = dv_isvalid_always_passes;

    test_assert_int_equal (dvalidtr_insert (&d, 0, dv_payload, 4, &ctx->e), SUCCESS);

    ext_array_free (&ref);
    ext_array_free (&sut);
  }

  TEST_CASE ("A failing hook fails the insert even though the writers agree")
  {
    struct ext_array ref = ext_array_create (ctx->mem);
    struct ext_array sut = ext_array_create (ctx->mem);
    struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);
    d.isvalid            = dv_isvalid_always_fails;

    test_err_t_check (dvalidtr_insert (&d, 0, dv_payload, 4, &ctx->e), ERR_CORRUPT, &ctx->e);

    ext_array_free (&ref);
    ext_array_free (&sut);
  }
}

TEST (dvalidtr_random_test_agreeing_writers)
{
  // Element sizes 1 and 4, so the size scaling in the random driver is covered
  const u32 sizes[] = {1, 4};

  for (u32 i = 0; i < arrlen (sizes); ++i) {
    TEST_CASE ("size=%u", sizes[i])
    {
      struct ext_array ref = ext_array_create (ctx->mem);
      struct ext_array sut = ext_array_create (ctx->mem);
      struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);

      test_assert_int_equal (dvalidtr_random_test (&d, sizes[i], 200, 64, &ctx->e), SUCCESS);

      ext_array_free (&ref);
      ext_array_free (&sut);
    }
  }
}

TEST (dvalidtr_random_test_reports_a_broken_sut)
{
  TEST_CASE ("A sut that diverged in content is reported")
  {
    struct ext_array ref = ext_array_create (ctx->mem);
    struct ext_array sut = ext_array_create (ctx->mem);
    struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);

    test_assert_int_equal (
        dvalidtr_insert (&d, 0, dv_payload, sizeof dv_payload, &ctx->e),
        SUCCESS
    );

    sut.data[0] ^= 0xFF;
    test_err_t_check (dvalidtr_random_test (&d, 1, 0, 16, &ctx->e), ERR_CORRUPT, &ctx->e);

    ext_array_free (&ref);
    ext_array_free (&sut);
  }

  TEST_CASE ("A sut that diverged in length is reported")
  {
    struct ext_array ref = ext_array_create (ctx->mem);
    struct ext_array sut = ext_array_create (ctx->mem);
    struct dvalidtr  d   = dv_create (&ref, &sut, ctx->mem);

    test_assert_int_equal (
        dvalidtr_insert (&d, 0, dv_payload, sizeof dv_payload, &ctx->e),
        SUCCESS
    );

    ext_array_insert (&sut, 0, dv_payload, 2, &ctx->e);
    test_assert_int_equal (ctx->e.cause_code, SUCCESS);

    test_err_t_check (dvalidtr_random_test (&d, 1, 0, 16, &ctx->e), ERR_CORRUPT, &ctx->e);

    ext_array_free (&ref);
    ext_array_free (&sut);
  }
}

/******************************************************************************
 * SECTION: Failure reporting
 * ----------------------------------------------------------------------------
 * The validator is the oracle the simulation tests trust, so the branches that
 * matter most are the ones that report a disagreement. [dv_stub] is a
 * data_writer over an ext_array that can be told to refuse an operation, or to
 * lie about how much it moved, so either side can be broken on purpose.
 *
 * error_causef only records a cause when the error is still clean, so a writer
 * that refused with ERR_IO keeps that code and the validator's ERR_CORRUPT is
 * only a trace line. ERR_CORRUPT is returned only where the validator itself
 * is the first to notice -- a count, data or length disagreement.
 ******************************************************************************/

enum
{
  DV_F_INSERT = 1 << 0,
  DV_F_READ   = 1 << 1,
  DV_F_WRITE  = 1 << 2,
  DV_F_REMOVE = 1 << 3,
  DV_F_GETLEN = 1 << 4,
  DV_F_ALLOPS = DV_F_INSERT | DV_F_READ | DV_F_WRITE | DV_F_REMOVE,
};

/// [fail] is a mask of DV_F_* operations that return an error instead of
/// forwarding. [delta] is added to the count a *successful* op returns, which
/// is how the "count mismatch" branches are reached without an error. getlen
/// stays honest under [delta] so the length checks still mean something.
struct dv_stub
{
  struct ext_array arr;
  u32              fail;
  i64              delta;
};

static err_t
dv_stub_insert (void *vctx, const u32 ofst, const void *src, const u32 slen, error *e)
{
  struct dv_stub *s = vctx;
  if (s->fail & DV_F_INSERT) {
    return error_causef (e, ERR_IO, "dv_stub: insert refused");
  }
  const i64 n = ext_array_insert (&s->arr, ofst, src, slen, e);
  return n < 0 ? (err_t)n : (err_t)(n + s->delta);
}

static i64
dv_stub_read (void *vctx, const struct stride str, const u32 size, void *dest, error *e)
{
  struct dv_stub *s = vctx;
  if (s->fail & DV_F_READ) {
    return error_causef (e, ERR_IO, "dv_stub: read refused");
  }
  return (i64)ext_array_read (&s->arr, str, size, dest) + s->delta;
}

static i64
dv_stub_write (void *vctx, const struct stride str, const u32 size, const void *src, error *e)
{
  struct dv_stub *s = vctx;
  if (s->fail & DV_F_WRITE) {
    return error_causef (e, ERR_IO, "dv_stub: write refused");
  }
  return (i64)ext_array_write (&s->arr, str, size, src) + s->delta;
}

static i64
dv_stub_remove (void *vctx, const struct stride str, const u32 size, void *dest, error *e)
{
  struct dv_stub *s = vctx;
  if (s->fail & DV_F_REMOVE) {
    return error_causef (e, ERR_IO, "dv_stub: remove refused");
  }
  return (i64)ext_array_remove (&s->arr, str, size, dest) + s->delta;
}

static i64
dv_stub_getlen (void *vctx, error *e)
{
  struct dv_stub *s = vctx;
  if (s->fail & DV_F_GETLEN) {
    return error_causef (e, ERR_IO, "dv_stub: getlen refused");
  }
  return (i64)ext_array_get_len (&s->arr);
}

static const struct data_writer_functions dv_stub_funcs = {
    .insert = dv_stub_insert,
    .read   = dv_stub_read,
    .write  = dv_stub_write,
    .remove = dv_stub_remove,
    .getlen = dv_stub_getlen,
};

/// A validator wired to two stubs, so a test can break the ref, the sut, or
/// neither. Both arrays start holding [dv_payload].
///
///   ref: [10 20 30 40 50 60 70 80]
///   sut: [10 20 30 40 50 60 70 80]
struct dv_pair
{
  struct dv_stub  ref;
  struct dv_stub  sut;
  struct dvalidtr d;
};

static void
dv_pair_init (struct dv_pair *p, const struct i_mem mem, error *e)
{
  p->ref = (struct dv_stub){.arr = ext_array_create (mem), .fail = 0, .delta = 0};
  p->sut = (struct dv_stub){.arr = ext_array_create (mem), .fail = 0, .delta = 0};

  ext_array_insert (&p->ref.arr, 0, dv_payload, sizeof dv_payload, e);
  ext_array_insert (&p->sut.arr, 0, dv_payload, sizeof dv_payload, e);

  p->d = (struct dvalidtr){
      .mem     = mem,
      .isvalid = NULL,
      .ref     = {.functions = dv_stub_funcs, .ctx = &p->ref},
      .sut     = {.functions = dv_stub_funcs, .ctx = &p->sut},
  };
}

static void
dv_pair_free (struct dv_pair *p)
{
  ext_array_free (&p->ref.arr);
  ext_array_free (&p->sut.arr);
}

static const struct stride dv_head = {.start = 0, .stride = 1, .nelems = 4};

TEST (dvalidtr_light_validate_propagates_getlen_failure)
{
  // Ordered: the sut is asked first, so a broken ref is only seen when the
  // sut answered.
  TEST_CASE ("a refusing sut getlen is propagated")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.fail = DV_F_GETLEN;

    test_err_t_check (dvalidtr_light_validate (&p.d, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a refusing ref getlen is propagated")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_GETLEN;

    test_err_t_check (dvalidtr_light_validate (&p.d, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }
}

TEST (dvalidtr_read_reports_failures)
{
  TEST_CASE ("a refusing ref read is reported as corruption")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_READ;

    test_err_t_check (dvalidtr_read (&p.d, dv_head, 1, NULL, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a refusing sut read is reported as corruption")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.fail = DV_F_READ;

    test_err_t_check (dvalidtr_read (&p.d, dv_head, 1, NULL, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  // Same bytes, different count: the count check has to fire before memcmp.
  TEST_CASE ("a sut that under-reports its read count is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.delta = -1;

    test_err_t_check (dvalidtr_read (&p.d, dv_head, 1, NULL, &ctx->e), ERR_CORRUPT, &ctx->e);
    dv_pair_free (&p);
  }
}

TEST (dvalidtr_insert_reports_failures)
{
  TEST_CASE ("a refusing ref insert is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_INSERT;

    test_err_t_check (dvalidtr_insert (&p.d, 0, dv_payload, 4, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a refusing sut insert is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.fail = DV_F_INSERT;

    test_err_t_check (dvalidtr_insert (&p.d, 0, dv_payload, 4, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a sut that over-reports its insert count is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.delta = 1;

    test_err_t_check (dvalidtr_insert (&p.d, 0, dv_payload, 4, &ctx->e), ERR_CORRUPT, &ctx->e);
    dv_pair_free (&p);
  }

  // Both inserts agree, so the failure can only surface in the read-back.
  TEST_CASE ("a read-back that cannot run fails the insert")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_READ;

    test_err_t_check (dvalidtr_insert (&p.d, 0, dv_payload, 4, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  // The read-back only covers the inserted range, so trailing extra bytes in
  // the sut are invisible to it and only the length check can catch them.
  //
  //   ref: [....8....] + insert 4 -> 12
  //   sut: [....8....][aa bb] + insert 4 -> 14
  TEST_CASE ("agreeing inserts over diverged lengths fail the length check")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);

    const u8 extra[2] = {0xAA, 0xBB};
    ext_array_insert (&p.sut.arr, sizeof dv_payload, extra, sizeof extra, &ctx->e);
    test_assert_int_equal (ctx->e.cause_code, SUCCESS);

    test_err_t_check (dvalidtr_insert (&p.d, 0, dv_payload, 4, &ctx->e), ERR_CORRUPT, &ctx->e);
    dv_pair_free (&p);
  }
}

TEST (dvalidtr_write_reports_failures)
{
  const u8 replacement[4] = {0x01, 0x02, 0x03, 0x04};

  // Ordered: the sut is written first, so a broken ref is only seen when the
  // sut accepted the write.
  TEST_CASE ("a refusing sut write is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.fail = DV_F_WRITE;

    test_err_t_check (dvalidtr_write (&p.d, dv_head, 1, replacement, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a refusing ref write is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_WRITE;

    test_err_t_check (dvalidtr_write (&p.d, dv_head, 1, replacement, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a sut that under-reports its write count is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.delta = -1;

    test_err_t_check (
        dvalidtr_write (&p.d, dv_head, 1, replacement, &ctx->e),
        ERR_CORRUPT,
        &ctx->e
    );
    dv_pair_free (&p);
  }

  TEST_CASE ("a read-back that cannot run fails the write")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_READ;

    test_err_t_check (dvalidtr_write (&p.d, dv_head, 1, replacement, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a failing isvalid hook fails the write")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.d.isvalid = dv_isvalid_always_fails;

    test_err_t_check (
        dvalidtr_write (&p.d, dv_head, 1, replacement, &ctx->e),
        ERR_CORRUPT,
        &ctx->e
    );
    dv_pair_free (&p);
  }
}

TEST (dvalidtr_remove_reports_failures)
{
  TEST_CASE ("a refusing ref remove is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_REMOVE;

    test_err_t_check (dvalidtr_remove (&p.d, dv_head, 1, NULL, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a refusing sut remove is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.fail = DV_F_REMOVE;

    test_err_t_check (dvalidtr_remove (&p.d, dv_head, 1, NULL, &ctx->e), ERR_IO, &ctx->e);
    dv_pair_free (&p);
  }

  TEST_CASE ("a sut that under-reports its remove count is reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.delta = -1;

    test_err_t_check (dvalidtr_remove (&p.d, dv_head, 1, NULL, &ctx->e), ERR_CORRUPT, &ctx->e);
    dv_pair_free (&p);
  }

  // Same count removed, different bytes handed back.
  TEST_CASE ("removed bytes that diverge are reported")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.sut.arr.data[1] ^= 0xFF;

    test_err_t_check (dvalidtr_remove (&p.d, dv_head, 1, NULL, &ctx->e), ERR_CORRUPT, &ctx->e);
    dv_pair_free (&p);
  }

  // The removed prefix matches, so only the post-remove length check sees the
  // trailing bytes the sut has and the ref does not.
  //
  //   ref: [....8....]    - remove 4 -> 4
  //   sut: [....8....][aa bb] - remove 4 -> 6
  TEST_CASE ("agreeing removes over diverged lengths fail the length check")
  {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);

    const u8 extra[2] = {0xAA, 0xBB};
    ext_array_insert (&p.sut.arr, sizeof dv_payload, extra, sizeof extra, &ctx->e);
    test_assert_int_equal (ctx->e.cause_code, SUCCESS);

    test_err_t_check (dvalidtr_remove (&p.d, dv_head, 1, NULL, &ctx->e), ERR_CORRUPT, &ctx->e);
    dv_pair_free (&p);
  }
}

// The random driver has a separate report per action, so every action has to be
// seen failing. The stub refuses all four, and the arrays start non-empty so
// the driver is free to pick any of them rather than being forced to insert.
TEST (dvalidtr_random_test_reports_every_failing_action)
{
  for (u32 iter = 0; iter < 40; ++iter) {
    struct dv_pair p;
    dv_pair_init (&p, ctx->mem, &ctx->e);
    p.ref.fail = DV_F_ALLOPS;
    p.sut.fail = DV_F_ALLOPS;

    test_err_t_check (dvalidtr_random_test (&p.d, 1, 1, 8, &ctx->e), ERR_CORRUPT, &ctx->e);
    dv_pair_free (&p);
  }
}

// getlen is the first call the driver makes, so it fails before any action.
TEST (dvalidtr_random_test_propagates_getlen_failure)
{
  struct dv_pair p;
  dv_pair_init (&p, ctx->mem, &ctx->e);
  p.ref.fail = DV_F_GETLEN;

  test_err_t_check (dvalidtr_random_test (&p.d, 1, 1, 8, &ctx->e), ERR_IO, &ctx->e);
  dv_pair_free (&p);
}

#endif
