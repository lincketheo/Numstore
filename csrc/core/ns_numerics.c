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

#include "core/ns_numerics.h"

#include "core/ns_bounds.h"
#include "core/ns_csx_assert.h"

#ifndef NDEBUG
#  include "core/ns_testing.h"
#endif

#include <inttypes.h>
#include <math.h>
#include <string.h>

float
f16_to_f32 (const u16 h)
{
  const u32 sign = (u32)(h >> 15) & 1U;
  u32       exp  = (u32)(h >> 10) & 0x1FU;
  u32       mant = (u32)h & 0x3FFU;
  u32       f;

  if (exp == 0) {
    if (mant == 0) {
      // Signed Zero
      f = (sign << 31);
    } else {
      // Subnormal f16 becomes a Normal f32
      // Shift mantissa until the first set bit is at position 10
      u32 e = 0;
      while (!(mant & 0x400U)) {
        mant <<= 1;
        e++;
      }
      // Remove the now-implicit leading bit (bit 10)
      mant &= 0x3FFU;
      // New exponent = bias offset (112) + 1 - shifts
      f = (sign << 31) | ((113 - e) << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    // Inf or NaN
    f = (sign << 31) | 0x7F800000U | (mant << 13);
  } else {
    // Normal numbers
    f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
  }

  float result;
  memcpy (&result, &f, 4);
  return result;
}

/**
static u32 crc32c_tbl[256];
static int crc32c_inited = 0;

static void
crc32c_init (void)
{
  for (u32 i = 0; i < 256; ++i) {
    u32 c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c >> 1) ^ (0x82F63B78U & -((int)(c & 1)));
    }
    crc32c_tbl[i] = c;
  }
  crc32c_inited = 1;
}

u32
checksum_init (void)
{
  return 0;
}

void
checksum_execute (u32 *state, const u8 *data, const u32 len)
{
  ASSERT (state);
  ASSERT (data);
  ASSERT (len > 0);

  if (!crc32c_inited) {
    crc32c_init ();
  }

  u32 c = ~(*state);
  for (u32 i = 0; i < len; ++i) {
    c = (c >> 8) ^ crc32c_tbl[(c ^ data[i]) & 0xFF];
  }
  *state = ~c;
}

*/

u8
randu8 (void)
{
  return (u8)rand ();
}

u32
randu32 (void)
{
  return (u32)rand ();
}

#ifndef NDEBUG
TEST (randu32)
{
  for (int i = 0; i < 1000; ++i) {
    const u32 v = randu32 ();
    test_assert (v <= (u32)RAND_MAX);
  }
}
#endif

u32
randu32r (const u32 lower, const u32 upper)
{
  ASSERT (upper >= lower);

  if (upper == lower) {
    return lower;
  }

  return (u32)randu64r ((u64)lower, (u64)upper);
}

#ifndef NDEBUG
TEST (randu32r)
{
  // lower == upper
  test_assert_type_equal (randu32r (5, 5), 5u, u32, "ud");
  test_assert_type_equal (randu32r (0, 0), 0u, u32, "ud");
  test_assert_type_equal (randu32r (U32_MAX, U32_MAX), U32_MAX, u32, "ud");

  // tiny range - both endpoints must be reachable
  {
    bool saw_lo = false;
    bool saw_hi = false;
    for (int i = 0; i < 1000; ++i) {
      const u32 v = randu32r (10, 11);
      test_assert (v == 10u || v == 11u);
      if (v == 10u) {
        saw_lo = true;
      }
      if (v == 11u) {
        saw_hi = true;
      }
    }
    test_assert (saw_lo);
    test_assert (saw_hi);
  }

  // full 32-bit range
  for (int i = 0; i < 10; ++i) {
    const u32 v = randu32r (0u, U32_MAX);
    test_assert (v <= U32_MAX);
  }

  // random ranges
  for (int i = 0; i < 10; ++i) {
    const u32 lo = (u32)(rand () % 10000);
    const u32 hi = lo + (u32)(rand () % 10000);
    const u32 v  = randu32r (lo, hi);
    test_assert (v >= lo);
    test_assert (v <= hi);
  }
}
#endif

i32
randi32r (const i32 lower, const i32 upper)
{
  ASSERT (upper >= lower);

  if (upper == lower) {
    return lower;
  }

  const u64 range = (u64)((i64)upper - (i64)lower) + 1U;
  return (i32)((u32)lower + (u32)randu64r (0, range - 1));
}

#ifndef NDEBUG
TEST (randi32r)
{
  test_assert_int_equal (randi32r (7, 7), 7);
  test_assert_int_equal (randi32r (I32_MIN, I32_MIN), I32_MIN);
  test_assert_int_equal (randi32r (I32_MAX, I32_MAX), I32_MAX);

  for (int i = 0; i < 100; ++i) {
    const i32 v = randi32r (-10, -9);
    test_assert (v == -10 || v == -9);
  }

  for (int i = 0; i < 100; ++i) {
    const i32 v = randi32r (I32_MIN, I32_MAX);
    test_assert (v >= I32_MIN);
    test_assert (v <= I32_MAX);
  }

  for (int i = 0; i < 100; ++i) {
    const i32 lo = (i32)(rand () % 10000) - 5000;
    const i32 hi = lo + (i32)(rand () % 10000);
    const i32 v  = randi32r (lo, hi);
    test_assert (v >= lo);
    test_assert (v <= hi);
  }
}
#endif

u64
randu64 (void)
{
  const u64 base     = (u64)RAND_MAX + 1u;
  u64       r        = (u64)rand ();
  u64       capacity = base;
  while (capacity <= (U64_MAX / base)) {
    r = (r * base) + (u64)rand ();
    capacity *= base;
  }
  r = (r * base) + (u64)rand ();
  return r;
}

u64
randu64r (const u64 lower, const u64 upper)
{
  ASSERT (upper >= lower);
  if (upper == lower) {
    return lower;
  }
  if (lower == 0 && upper == U64_MAX) {
    return randu64 ();
  }
  const u64 range = upper - lower + 1U;

#if defined(_MSC_VER) && defined(_M_X64)
  // 64-bit MSVC: _umul128 intrinsic available
  u64 x = randu64 ();
  u64 hi;
  u64 lo = _umul128 (x, range, &hi);
  if (lo < range) {
    const u64 t = (-range) % range;
    while (lo < t) {
      x  = randu64 ();
      lo = _umul128 (x, range, &hi);
    }
  }
  return lower + hi;

#elif defined(__SIZEOF_INT128__)
  // GCC/Clang (Linux, macOS, mingw-w64)
  u64         x = randu64 ();
  __uint128_t m = (__uint128_t)x * range;
  u64         l = (u64)m;
  if (l < range) {
    const u64 t = (-range) % range;
    while (l < t) {
      x = randu64 ();
      m = (__uint128_t)x * range;
      l = (u64)m;
    }
  }
  return lower + (u64)(m >> 64);

#else
// 32-bit MSVC (win32): emulate 64x64->128 multiply using 32-bit halves
// Decomposes a*b into (a_hi*2^32 + a_lo) * (b_hi*2^32 + b_lo)
#  define HI32(v) ((u64)((v) >> 32))
#  define LO32(v) ((u64)(u32)(v))

  u64 x  = randu64 ();
  // compute hi = upper 64 bits of x * range
  u64 hi = HI32 (x) * HI32 (range)
           + (HI32 (HI32 (x) * LO32 (range) + HI32 (LO32 (x) * LO32 (range)))
              + HI32 (LO32 (x) * HI32 (range)));
  u64 lo = x * range; // lower 64 bits (natural wraparound)

  if (lo < range) {
    const u64 t = (-range) % range;
    while (lo < t) {
      x  = randu64 ();
      hi = HI32 (x) * HI32 (range)
           + (HI32 (HI32 (x) * LO32 (range) + HI32 (LO32 (x) * LO32 (range)))
              + HI32 (LO32 (x) * HI32 (range)));
      lo = x * range;
    }
  }
  return lower + hi;

#  undef HI32
#  undef LO32
#endif
}

u64
randu64e (const u64 lower, const u64 upper)
{
  ASSERT (upper > lower);
  return randu64r (lower, upper - 1);
}

#ifndef NDEBUG
TEST (randu64r)
{
  test_assert_type_equal (
      randu64r (1234567890123ull, 1234567890123ull),
      1234567890123ull,
      u64,
      PRIu64
  );
  test_assert_type_equal (randu64r (0ull, 0ull), 0ull, u64, PRIu64);
  test_assert_type_equal (randu64r (U64_MAX, U64_MAX), U64_MAX, u64, PRIu64);

  // Both endpoints reachable
  {
    bool saw_lo = false;
    bool saw_hi = false;
    for (int i = 0; i < 1000; ++i) {
      const u64 v = randu64r (1000ull, 1001ull);
      test_assert (v == 1000ull || v == 1001ull);
      if (v == 1000ull) {
        saw_lo = true;
      }
      if (v == 1001ull) {
        saw_hi = true;
      }
    }
    test_assert (saw_lo);
    test_assert (saw_hi);
  }

  // Full range
  for (int i = 0; i < 100; ++i) {
    const u64 v = randu64r (0ull, U64_MAX);
    test_assert (v <= U64_MAX);
  }

  for (int i = 0; i < 50; ++i) {
    const u64 lo = ((u64)randu32 () << 16);
    const u64 hi = lo + (u64)(randu32 () % 100000);
    const u64 v  = randu64r (lo, hi);
    test_assert (v >= lo);
    test_assert (v <= hi);
  }
}

TEST (randu64e)
{
  // Both endpoints of the exclusive range reachable
  {
    bool saw_lo = false;
    bool saw_hi = false;
    for (int i = 0; i < 1000; ++i) {
      const u64 v = randu64e (10ull, 12ull);
      test_assert (v == 10ull || v == 11ull);
      if (v == 10ull) {
        saw_lo = true;
      }
      if (v == 11ull) {
        saw_hi = true;
      }
    }
    test_assert (saw_lo);
    test_assert (saw_hi);
  }

  for (int i = 0; i < 50; ++i) {
    const u64 lo = (u64)(randu32 () % 1000);
    const u64 hi = lo + (u64)(randu32 () % 1000) + 1u;
    const u64 v  = randu64e (lo, hi);
    test_assert (v >= lo && v < hi);
  }
}
#endif

i64
randi64r (const i64 lower, const i64 upper)
{
  ASSERT (upper >= lower);

  if (upper == lower) {
    return lower;
  }

  // range via u64 subtraction. Overflows to 0 only for lower=I64_MIN,
  // upper=I64_MAX.
  const u64 range = (u64)upper - (u64)lower + 1U;
  if (range == 0) {
    return (i64)randu64 ();
  }

  return (i64)((u64)lower + randu64r (0, range - 1));
}

i64
randi64e (const i64 lower, const i64 upper)
{
  ASSERT (upper > lower);
  return randi64r (lower, upper - 1);
}

#ifndef NDEBUG
TEST (randi64r)
{
  test_assert_type_equal (randi64r (7, 7), 7, i64, PRId64);
  test_assert_type_equal (randi64r (I64_MIN, I64_MIN), I64_MIN, i64, PRId64);
  test_assert_type_equal (randi64r (I64_MAX, I64_MAX), I64_MAX, i64, PRId64);

  // Full i64 range
  for (int i = 0; i < 100; ++i) {
    const i64 v = randi64r (I64_MIN, I64_MAX);
    test_assert (v >= I64_MIN && v <= I64_MAX);
  }

  for (int i = 0; i < 10; ++i) {
    const i64 lo = (i64)(rand () % 100000) - 50000;
    const i64 hi = lo + (i64)(rand () % 100000);
    const i64 v  = randi64r (lo, hi);
    test_assert (v >= lo && v <= hi);
  }
}

TEST (randi64e)
{
  {
    bool saw_lo = false;
    bool saw_hi = false;
    for (int i = 0; i < 1000; ++i) {
      const i64 v = randi64e (-5, -3);
      test_assert (v == -5 || v == -4);
      if (v == -5) {
        saw_lo = true;
      }
      if (v == -4) {
        saw_hi = true;
      }
    }
    test_assert (saw_lo);
    test_assert (saw_hi);
  }

  for (int i = 0; i < 50; ++i) {
    const i64 lo = (i64)(rand () % 1000) - 500;
    const i64 hi = lo + (i64)(rand () % 1000) + 1;
    const i64 v  = randi64e (lo, hi);
    test_assert (v >= lo && v < hi);
  }
}
#endif

f32
randf (void)
{
  return (f32)rand () / (f32)RAND_MAX;
}

#ifndef NDEBUG
TEST (randf)
{
  for (int i = 0; i < 1000; ++i) {
    f32 f = randf ();
    test_assert (f >= 0 && f <= 1);
  }
}
#endif

void
rand_bytes (void *dst, const u32 n)
{
  // Byte stepping needs a sized type: arithmetic on void * is a GNU extension
  // that MSVC rejects outright (C2036).
  u8    *d = dst;
  u64    s = (((u64)rand () << 32) ^ (u64)rand ()) | 1;
  b_size i = 0;
  for (; i + 8 <= n; i += 8) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    memcpy (d + i, &s, 8);
  }
  if (i < n) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    memcpy (d + i, &s, n - i);
  }
}

err_t
parse_i64_expect (i64 *dest, const char *data, const u32 len, error *e)
{
  ASSERT (data);
  ASSERT (len > 0);
  ASSERT (dest);

  u32  i   = 0;
  bool neg = false;

  if (data[i] == '+' || data[i] == '-') {
    neg = (data[i] == '-');
    i++;
    ASSERT (i < len);
  }

  i64 acc = 0;

  for (; i < len; i++) {
    const char c = data[i];
    ASSERT (is_num (c));

    const i64 digit = c - '0';

    if (!safe_mul_i64 (&acc, acc, 10L)) {
      goto failed;
    }

    if (!safe_sub_i64 (&acc, acc, digit)) {
      goto failed;
    }
  }

  if (!neg) {
    if (acc == I64_MIN) {
      goto failed;
    }
    acc = -acc;
  }

  *dest = acc;
  return SUCCESS;

failed:
  return error_causef (e, ERR_ARITH, "i64 overflow");
}

err_t
parse_i32_expect (i32 *dest, const char *data, const u32 len, error *e)
{
  ASSERT (data);
  ASSERT (len > 0);
  ASSERT (dest);

  u32  i   = 0;
  bool neg = false;

  if (data[i] == '+' || data[i] == '-') {
    neg = (data[i] == '-');
    i++;
    ASSERT (i < len); // We expect string to be valid
  }

  i32 acc = 0;

  for (; i < len; i++) {
    const char c = data[i];
    ASSERT (is_num (c));

    const i32 digit = c - '0';

    if (!safe_mul_i32 (&acc, acc, 10)) {
      goto failed;
    }

    if (!safe_sub_i32 (&acc, acc, digit)) {
      goto failed;
    }
  }

  if (!neg) {
    if (acc == I32_MIN) {
      goto failed;
    }
    acc = -acc;
  }

  *dest = acc;
  return SUCCESS;

failed:
  return error_causef (e, ERR_ARITH, "i32 overflow");
}

#ifndef NDEBUG
TEST (parse_i32_expect)
{
  i32 out = -1;

  test_assert_int_equal (parse_i32_expect (&out, "1234", 4, &ctx->e), SUCCESS);
  test_assert_int_equal (out, 1234);

  test_assert_int_equal (parse_i32_expect (&out, "-56", 3, &ctx->e), SUCCESS);
  test_assert_int_equal (out, -56);

  const char *big = "999999999999999999999999999999999999999999";
  test_assert_int_equal (parse_i32_expect (&out, big, strlen (big), &ctx->e), ERR_ARITH);
}
#endif

err_t
parse_f32_expect (f32 *dest, const char *s, const u32 len, error *e)
{
  ASSERT (s);
  ASSERT (dest);
  ASSERT (len > 0);

  u32  i   = 0;
  bool neg = false;

  if (s[i] == '+' || s[i] == '-') {
    neg = (s[i] == '-');
    i++;
    ASSERT (i < len);
  }

  // Integer part
  f32  acc       = 0.0F;
  bool saw_digit = false;
  (void)saw_digit; // Unused in release
  while (i < len && s[i] >= '0' && s[i] <= '9') {
    const f32 d = (f32)(s[i] - '0');
    if (!safe_mul_f32 (&acc, acc, 10.0F)) {
      goto failed;
    }
    if (!safe_add_f32 (&acc, acc, d)) {
      goto failed;
    }
    i++;
    saw_digit = true;
  }

  // Fractional part
  if (i < len && s[i] == '.') {
    i++;
    ASSERT (i < len); // cannot end with '.'
    f32 frac  = 0.0f;
    f32 scale = 1.0f;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      const f32 d = (f32)(s[i] - '0');
      if (!safe_mul_f32 (&frac, frac, 10.0F)) {
        goto failed;
      }
      if (!safe_add_f32 (&frac, frac, d)) {
        goto failed;
      }
      if (!safe_mul_f32 (&scale, scale, 10.0F)) {
        goto failed;
      }
      i++;
      saw_digit = true;
    }
    f32 tmp;
    if (!safe_div_f32 (&tmp, frac, scale)) {
      goto failed;
    }
    if (!safe_add_f32 (&acc, acc, tmp)) {
      goto failed;
    }
  }

  ASSERT (saw_digit);

  // Exponent part
  if (i < len && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    ASSERT (i < len); // must have exponent digits
    bool exp_neg = false;
    if (s[i] == '+' || s[i] == '-') {
      exp_neg = (s[i] == '-');
      i++;
      ASSERT (i < len);
    }
    u32  exp     = 0;
    bool saw_exp = false;
    (void)saw_exp; // Unused in release
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      const u32 d = (u32)(s[i] - '0');
      ASSERT (exp <= (UINT32_MAX - d) / 10);
      exp = (exp * 10) + d;
      i++;
      saw_exp = true;
    }
    ASSERT (saw_exp);

    // Apply exponent
    for (u32 k = 0; k < exp; k++) {
      if (exp_neg) {
        if (!safe_div_f32 (&acc, acc, 10.0F)) {
          goto failed;
        }
      } else {
        if (!safe_mul_f32 (&acc, acc, 10.0F)) {
          goto failed;
        }
      }
    }
  }

  ASSERT (i == len); // no extra characters

  if (neg) {
    acc = -acc;
  }
  *dest = acc;
  return SUCCESS;

failed:
  return error_causef (e, ERR_ARITH, "f32 overflow");
}

#define EPSILON 1e-6f

#ifndef NDEBUG
TEST (parse_f32_expect)
{
  f32 out = NAN;

  test_assert_int_equal (parse_f32_expect (&out, "3.14", 4, &ctx->e), SUCCESS);
  test_assert_int_equal (fabsf (out - 3.14f) < EPSILON, true);

  test_assert_int_equal (parse_f32_expect (&out, "-0.5", 4, &ctx->e), SUCCESS);
  test_assert_int_equal (fabsf (out + 0.5f) < EPSILON, true);

  test_assert_int_equal (parse_f32_expect (&out, "1.23e3", 6, &ctx->e), SUCCESS);
  test_assert_int_equal (fabsf (out - 1230.0f) < EPSILON, true);

  test_assert_int_equal (parse_f32_expect (&out, ".25", 3, &ctx->e), SUCCESS);
  test_assert_int_equal (fabsf (out - 0.25f) < EPSILON, true);

  test_assert_int_equal (parse_f32_expect (&out, "1e40", 4, &ctx->e), ERR_ARITH);
}
#endif

float
py_mod_f32 (const float num, const float denom)
{
  if (denom == 0.0F) {
    return INFINITY;
  }

  float rem = num - (denom * (int)(num / denom));

  if ((rem < 0.0F && denom > 0.0F) || (rem > 0.0F && denom < 0.0F)) {
    rem += denom;
  }

  return rem;
}

#ifndef NDEBUG
TEST (py_mod_f32)
{
  // +num , +denom  (generic)
  test_assert (py_mod_f32 (5.5f, 2.0f) == 1.5f);

  // –num , +denom  (Python keeps denom’s sign)
  test_assert (py_mod_f32 (-5.5f, 2.0f) == 0.5f);

  // +num , –denom
  test_assert (py_mod_f32 (5.5f, -2.0f) == -0.5f);

  // –num , –denom
  test_assert (py_mod_f32 (-5.5f, -2.0f) == -1.5f);

  // exact multiple (remainder 0)
  test_assert (py_mod_f32 (4.0f, 2.0f) == 0.0f);

  // zero numerator
  test_assert (py_mod_f32 (0.0f, 3.3f) == 0.0f);

  // denominator 0 ⇒ +INF  (sentinel-style)
  test_assert (isinf (py_mod_f32 (7.0f, 0.0f)));
}
#endif

i32
py_mod_i32 (const i32 num, const i32 denom)
{
  i32 r = num % denom;
  if ((r != 0) && ((r < 0) != (denom < 0))) {
    r += denom;
  }
  return r;
}

#ifndef NDEBUG
TEST (py_mod_i32)
{
  // +num , +denom
  test_assert (py_mod_i32 (5, 3) == 2);

  // –num , +denom
  test_assert (py_mod_i32 (-5, 3) == 1);

  // +num , –denom
  test_assert (py_mod_i32 (5, -3) == -1);

  // –num , –denom
  test_assert (py_mod_i32 (-5, -3) == -2);

  // exact multiple (remainder 0)
  test_assert (py_mod_i32 (9, 3) == 0);

  // zero numerator
  test_assert (py_mod_i32 (0, 7) == 0);
}
#endif
