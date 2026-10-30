#include "core/os/ns_os.h"

////////////////////////////
/// Composite file helpers
///
/// Built only out of vtable primitives - they are here so the short read
/// check isn't copy pasted at every call site.

/// pread exactly [n] bytes - a short read is ERR_CORRUPT, not EOF
err_t
file_pread_all_expect (i_file f, void *dest, const u64 n, const u64 offset, error *e)
{
  const i64 ret = f.table->pread_all (f.self, dest, n, offset, e);
  if (ret < 0) {
    return error_trace (e);
  }

  if (unlikely ((u64)ret != n)) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "pread: short read (got %" PRId64 " of %" PRId64 " bytes)",
        ret,
        (i64)n
    );
  }

  return SUCCESS;
}

/// read exactly [nbytes] bytes - a short read is ERR_CORRUPT, not EOF
err_t
file_read_all_expect (i_file f, void *dest, const u64 nbytes, error *e)
{
  const i64 ret = f.table->read_all (f.self, dest, nbytes, e);
  if (ret < 0) {
    return error_trace (e);
  }

  if (unlikely ((u64)ret != nbytes)) {
    return error_causef (
        e,
        ERR_CORRUPT,
        "read: short read (got %" PRId64 " of %" PRId64 " bytes)",
        ret,
        (i64)nbytes
    );
  }

  return SUCCESS;
}

#ifndef NDEBUG
#  include "core/ns_testing.h"

#  define OS_T_FILE "ns_os_expect_test.bin"

// 16 bytes on disk, so a 16 byte read is exact and a 17 byte read is short
static const u8 os_t_payload[16] = {
    0x00,
    0x11,
    0x22,
    0x33,
    0x44,
    0x55,
    0x66,
    0x77,
    0x88,
    0x99,
    0xAA,
    0xBB,
    0xCC,
    0xDD,
    0xEE,
    0xFF,
};

static void
os_t_write_fixture (struct test_ctx *ctx)
{
  ctx->os.table->remove_quiet (ctx->os.self, OS_T_FILE, &ctx->e);

  i_file f;
  ctx->os.table->open_file_rw (ctx->os.self, &f, OS_T_FILE, &ctx->e);
  f.table->write_all (f.self, os_t_payload, sizeof os_t_payload, &ctx->e);
  ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e);
}

TEST (file_read_all_expect_exact_and_short)
{
  os_t_write_fixture (ctx);
  test_assert_int_equal (ctx->e.cause_code, SUCCESS);

  TEST_CASE ("Reading exactly the file length succeeds and fills the buffer")
  {
    i_file f;
    test_assert_int_equal (
        ctx->os.table->open_file_r (ctx->os.self, &f, OS_T_FILE, &ctx->e),
        SUCCESS
    );

    u8 got[16] = {0};
    test_assert_int_equal (file_read_all_expect (f, got, sizeof got, &ctx->e), SUCCESS);
    test_assert_memequal (got, os_t_payload, sizeof os_t_payload);

    test_assert_int_equal (ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e), SUCCESS);
  }

  TEST_CASE ("Reading one byte more than exists is ERR_CORRUPT, not EOF")
  {
    i_file f;
    test_assert_int_equal (
        ctx->os.table->open_file_r (ctx->os.self, &f, OS_T_FILE, &ctx->e),
        SUCCESS
    );

    u8 got[17] = {0};
    test_err_t_check (file_read_all_expect (f, got, sizeof got, &ctx->e), ERR_CORRUPT, &ctx->e);

    test_assert_int_equal (ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e), SUCCESS);
  }

  TEST_CASE ("A sequence of exact reads walks the file")
  {
    i_file f;
    test_assert_int_equal (
        ctx->os.table->open_file_r (ctx->os.self, &f, OS_T_FILE, &ctx->e),
        SUCCESS
    );

    u8 first[8]  = {0};
    u8 second[8] = {0};
    test_assert_int_equal (file_read_all_expect (f, first, 8, &ctx->e), SUCCESS);
    test_assert_int_equal (file_read_all_expect (f, second, 8, &ctx->e), SUCCESS);
    test_assert_memequal (first, os_t_payload, 8);
    test_assert_memequal (second, os_t_payload + 8, 8);

    // The file is used up, so the next read is short
    u8 third[8] = {0};
    test_err_t_check (file_read_all_expect (f, third, 8, &ctx->e), ERR_CORRUPT, &ctx->e);

    test_assert_int_equal (ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e), SUCCESS);
  }

  test_assert_int_equal (ctx->os.table->unlink (ctx->os.self, OS_T_FILE, &ctx->e), SUCCESS);
}

TEST (file_pread_all_expect_exact_and_short)
{
  os_t_write_fixture (ctx);
  test_assert_int_equal (ctx->e.cause_code, SUCCESS);

  i_file f;
  test_assert_int_equal (
      ctx->os.table->open_file_r (ctx->os.self, &f, OS_T_FILE, &ctx->e),
      SUCCESS
  );

  TEST_CASE ("An exact read at offset zero succeeds")
  {
    u8 got[16] = {0};
    test_assert_int_equal (file_pread_all_expect (f, got, sizeof got, 0, &ctx->e), SUCCESS);
    test_assert_memequal (got, os_t_payload, sizeof os_t_payload);
  }

  TEST_CASE ("An exact read at an interior offset succeeds")
  {
    u8 got[4] = {0};
    test_assert_int_equal (file_pread_all_expect (f, got, sizeof got, 8, &ctx->e), SUCCESS);
    test_assert_memequal (got, os_t_payload + 8, 4);
  }

  TEST_CASE ("The last byte of the file is readable")
  {
    u8 got = 0;
    test_assert_int_equal (file_pread_all_expect (f, &got, 1, 15, &ctx->e), SUCCESS);
    test_assert_type_equal (got, 0xFFu, u8, PRIu8);
  }

  TEST_CASE ("A read straddling the end of the file is ERR_CORRUPT")
  {
    u8 got[8] = {0};
    test_err_t_check (
        file_pread_all_expect (f, got, sizeof got, 12, &ctx->e),
        ERR_CORRUPT,
        &ctx->e
    );
  }

  TEST_CASE ("A read starting past the end of the file is ERR_CORRUPT")
  {
    u8 got[4] = {0};
    test_err_t_check (
        file_pread_all_expect (f, got, sizeof got, 1024, &ctx->e),
        ERR_CORRUPT,
        &ctx->e
    );
  }

  test_assert_int_equal (ctx->os.table->close_file (ctx->os.self, f.self, &ctx->e), SUCCESS);
  test_assert_int_equal (ctx->os.table->unlink (ctx->os.self, OS_T_FILE, &ctx->e), SUCCESS);
}

// The ret < 0 branch: the primitive itself fails rather than coming up short
TEST (file_all_expect_propagates_an_underlying_failure)
{
  os_t_write_fixture (ctx);
  test_assert_int_equal (ctx->e.cause_code, SUCCESS);

  i_file f;
  test_assert_int_equal (
      ctx->faulty_os.table->open_file_r (ctx->faulty_os.self, &f, OS_T_FILE, &ctx->e),
      SUCCESS
  );

  TEST_CASE ("file_read_all_expect surfaces the failure")
  {
    faulty_os_fail_after (ctx->faulty_os, 0);

    u8 got[16] = {0};
    test_assert (file_read_all_expect (f, got, sizeof got, &ctx->e) != SUCCESS);
    error_reset (&ctx->e);
  }

  TEST_CASE ("file_pread_all_expect surfaces the failure")
  {
    faulty_os_fail_after (ctx->faulty_os, 0);

    u8 got[16] = {0};
    test_assert (file_pread_all_expect (f, got, sizeof got, 0, &ctx->e) != SUCCESS);
    error_reset (&ctx->e);
  }

  // Let the close and unlink through
  faulty_os_set_fail (ctx->faulty_os, 0.0f);

  test_assert_int_equal (
      ctx->faulty_os.table->close_file (ctx->faulty_os.self, f.self, &ctx->e),
      SUCCESS
  );
  test_assert_int_equal (ctx->os.table->unlink (ctx->os.self, OS_T_FILE, &ctx->e), SUCCESS);
}

// [ncalls] is bumped by the roll, which every fallible call goes through -
// so the counter is a count of calls of any kind, not of failures.
TEST (faulty_num_os_calls)
{
  struct i_os f = ctx->faulty_os;

  TEST_CASE ("A fallible call bumps the counter")
  {
    const u32 before = faulty_num_os_calls (f);

    i_mutex   m;
    test_assert_int_equal (f.table->mutex_create (f.self, &m, &ctx->e), SUCCESS);
    test_assert_int_equal (faulty_num_os_calls (f), before + 1);

    f.table->mutex_free (f.self, m.self);
  }

  TEST_CASE ("A failed call is still a call")
  {
    const u32 before = faulty_num_os_calls (f);

    faulty_os_fail_after (f, 0);

    i_mutex m;
    test_assert (f.table->mutex_create (f.self, &m, &ctx->e) != SUCCESS);
    error_reset (&ctx->e);

    test_assert_int_equal (faulty_num_os_calls (f), before + 1);
  }

  faulty_os_set_fail (f, 0.0f);
}
#endif // NDEBUG
