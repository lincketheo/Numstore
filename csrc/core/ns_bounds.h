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

#ifndef BOUNDS_H
#define BOUNDS_H

#include "core/ns_error.h"
#include "core/ns_platform.h"
#include "core/ns_stdtypes.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/******************************************************************************
 * Checked arithmetic
 *
 *   bool safe_<add|sub|mul|div>_<type> (T *dest, T a, T b);
 *
 * Returns true on success, false on overflow (or divide by zero).
 * Arguments are taken at the named width, so pass matching types.
 * On add/sub/mul failure, *dest holds the wrapped result.
 *****************************************************************************/

#if defined(__has_builtin)
#  if __has_builtin(__builtin_add_overflow)
#    define BOUNDS_USE_BUILTINS 1
#  endif
#elif defined(__GNUC__) && (__GNUC__ >= 5)
#  define BOUNDS_USE_BUILTINS 1
#endif

/*-----------------------------------------------------------------------------
 * GCC / Clang / clang-cl: compiler builtins
 *----------------------------------------------------------------------------*/
#ifdef BOUNDS_USE_BUILTINS

HEADER_FUNC bool
safe_add_u8 (u8 *d, u8 a, u8 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_u8 (u8 *d, u8 a, u8 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_u8 (u8 *d, u8 a, u8 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

HEADER_FUNC bool
safe_add_u16 (u16 *d, u16 a, u16 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_u16 (u16 *d, u16 a, u16 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_u16 (u16 *d, u16 a, u16 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

HEADER_FUNC bool
safe_add_u32 (u32 *d, u32 a, u32 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_u32 (u32 *d, u32 a, u32 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_u32 (u32 *d, u32 a, u32 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

HEADER_FUNC bool
safe_add_u64 (u64 *d, u64 a, u64 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_u64 (u64 *d, u64 a, u64 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_u64 (u64 *d, u64 a, u64 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

HEADER_FUNC bool
safe_add_i8 (i8 *d, i8 a, i8 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_i8 (i8 *d, i8 a, i8 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_i8 (i8 *d, i8 a, i8 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

HEADER_FUNC bool
safe_add_i16 (i16 *d, i16 a, i16 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_i16 (i16 *d, i16 a, i16 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_i16 (i16 *d, i16 a, i16 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

HEADER_FUNC bool
safe_add_i32 (i32 *d, i32 a, i32 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_i32 (i32 *d, i32 a, i32 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_i32 (i32 *d, i32 a, i32 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

HEADER_FUNC bool
safe_add_i64 (i64 *d, i64 a, i64 b)
{
  return !__builtin_add_overflow (a, b, d);
}

HEADER_FUNC bool
safe_sub_i64 (i64 *d, i64 a, i64 b)
{
  return !__builtin_sub_overflow (a, b, d);
}

HEADER_FUNC bool
safe_mul_i64 (i64 *d, i64 a, i64 b)
{
  return !__builtin_mul_overflow (a, b, d);
}

/*-----------------------------------------------------------------------------
 * MSVC / anything else: plain C
 *
 * 8/16/32-bit types: do the math in 64 bits, then check the range.
 * 64-bit types: check before computing so nothing overflows.
 *----------------------------------------------------------------------------*/
#else

HEADER_FUNC bool
safe_add_u8 (u8 *d, u8 a, u8 b)
{
  u64 r = (u64)a + b;
  *d    = (u8)r;
  return r <= UINT8_MAX;
}

HEADER_FUNC bool
safe_sub_u8 (u8 *d, u8 a, u8 b)
{
  *d = (u8)(a - b);
  return a >= b;
}

HEADER_FUNC bool
safe_mul_u8 (u8 *d, u8 a, u8 b)
{
  u64 r = (u64)a * b;
  *d    = (u8)r;
  return r <= UINT8_MAX;
}

HEADER_FUNC bool
safe_add_u16 (u16 *d, u16 a, u16 b)
{
  u64 r = (u64)a + b;
  *d    = (u16)r;
  return r <= UINT16_MAX;
}

HEADER_FUNC bool
safe_sub_u16 (u16 *d, u16 a, u16 b)
{
  *d = (u16)(a - b);
  return a >= b;
}

HEADER_FUNC bool
safe_mul_u16 (u16 *d, u16 a, u16 b)
{
  u64 r = (u64)a * b;
  *d    = (u16)r;
  return r <= UINT16_MAX;
}

HEADER_FUNC bool
safe_add_u32 (u32 *d, u32 a, u32 b)
{
  u64 r = (u64)a + b;
  *d    = (u32)r;
  return r <= UINT32_MAX;
}

HEADER_FUNC bool
safe_sub_u32 (u32 *d, u32 a, u32 b)
{
  *d = a - b;
  return a >= b;
}

HEADER_FUNC bool
safe_mul_u32 (u32 *d, u32 a, u32 b)
{
  u64 r = (u64)a * b;
  *d    = (u32)r;
  return r <= UINT32_MAX;
}

HEADER_FUNC bool
safe_add_u64 (u64 *d, u64 a, u64 b)
{
  *d = a + b;
  return *d >= a;
}

HEADER_FUNC bool
safe_sub_u64 (u64 *d, u64 a, u64 b)
{
  *d = a - b;
  return a >= b;
}

HEADER_FUNC bool
safe_mul_u64 (u64 *d, u64 a, u64 b)
{
  *d = a * b;
  return a == 0 || b <= UINT64_MAX / a;
}

HEADER_FUNC bool
safe_add_i8 (i8 *d, i8 a, i8 b)
{
  i64 r = (i64)a + b;
  *d    = (i8)r;
  return r >= INT8_MIN && r <= INT8_MAX;
}

HEADER_FUNC bool
safe_sub_i8 (i8 *d, i8 a, i8 b)
{
  i64 r = (i64)a - b;
  *d    = (i8)r;
  return r >= INT8_MIN && r <= INT8_MAX;
}

HEADER_FUNC bool
safe_mul_i8 (i8 *d, i8 a, i8 b)
{
  i64 r = (i64)a * b;
  *d    = (i8)r;
  return r >= INT8_MIN && r <= INT8_MAX;
}

HEADER_FUNC bool
safe_add_i16 (i16 *d, i16 a, i16 b)
{
  i64 r = (i64)a + b;
  *d    = (i16)r;
  return r >= INT16_MIN && r <= INT16_MAX;
}

HEADER_FUNC bool
safe_sub_i16 (i16 *d, i16 a, i16 b)
{
  i64 r = (i64)a - b;
  *d    = (i16)r;
  return r >= INT16_MIN && r <= INT16_MAX;
}

HEADER_FUNC bool
safe_mul_i16 (i16 *d, i16 a, i16 b)
{
  i64 r = (i64)a * b;
  *d    = (i16)r;
  return r >= INT16_MIN && r <= INT16_MAX;
}

HEADER_FUNC bool
safe_add_i32 (i32 *d, i32 a, i32 b)
{
  i64 r = (i64)a + b;
  *d    = (i32)r;
  return r >= INT32_MIN && r <= INT32_MAX;
}

HEADER_FUNC bool
safe_sub_i32 (i32 *d, i32 a, i32 b)
{
  i64 r = (i64)a - b;
  *d    = (i32)r;
  return r >= INT32_MIN && r <= INT32_MAX;
}

HEADER_FUNC bool
safe_mul_i32 (i32 *d, i32 a, i32 b)
{
  i64 r = (i64)a * b;
  *d    = (i32)r;
  return r >= INT32_MIN && r <= INT32_MAX;
}

HEADER_FUNC bool
safe_add_i64 (i64 *d, i64 a, i64 b)
{
  bool ok = (b > 0) ? (a <= INT64_MAX - b) : (a >= INT64_MIN - b);
  *d      = (i64)((u64)a + (u64)b);
  return ok;
}

HEADER_FUNC bool
safe_sub_i64 (i64 *d, i64 a, i64 b)
{
  bool ok = (b > 0) ? (a >= INT64_MIN + b) : (a <= INT64_MAX + b);
  *d      = (i64)((u64)a - (u64)b);
  return ok;
}

HEADER_FUNC bool
safe_mul_i64 (i64 *d, i64 a, i64 b)
{
  bool ok;
  if (a == 0 || b == 0) {
    ok = true;
  } else if (a > 0 && b > 0) {
    ok = a <= INT64_MAX / b;
  } else if (a > 0 && b < 0) {
    ok = b >= INT64_MIN / a;
  } else if (a < 0 && b > 0) {
    ok = a >= INT64_MIN / b;
  } else {
    ok = b >= INT64_MAX / a; // both negative
  }
  *d = (i64)((u64)a * (u64)b);
  return ok;
}

#endif

/*-----------------------------------------------------------------------------
 * Division
 *----------------------------------------------------------------------------*/

HEADER_FUNC bool
safe_div_u8 (u8 *d, u8 a, u8 b)
{
  if (b == 0) {
    return false;
  }
  *d = (u8)(a / b);
  return true;
}

HEADER_FUNC bool
safe_div_u16 (u16 *d, u16 a, u16 b)
{
  if (b == 0) {
    return false;
  }
  *d = (u16)(a / b);
  return true;
}

HEADER_FUNC bool
safe_div_u32 (u32 *d, u32 a, u32 b)
{
  if (b == 0) {
    return false;
  }
  *d = a / b;
  return true;
}

HEADER_FUNC bool
safe_div_u64 (u64 *d, u64 a, u64 b)
{
  if (b == 0) {
    return false;
  }
  *d = a / b;
  return true;
}

// Signed: also reject MIN / -1, which overflows
HEADER_FUNC bool
safe_div_i8 (i8 *d, i8 a, i8 b)
{
  if (b == 0 || (a == INT8_MIN && b == -1)) {
    return false;
  }
  *d = (i8)(a / b);
  return true;
}

HEADER_FUNC bool
safe_div_i16 (i16 *d, i16 a, i16 b)
{
  if (b == 0 || (a == INT16_MIN && b == -1)) {
    return false;
  }
  *d = (i16)(a / b);
  return true;
}

HEADER_FUNC bool
safe_div_i32 (i32 *d, i32 a, i32 b)
{
  if (b == 0 || (a == INT32_MIN && b == -1)) {
    return false;
  }
  *d = a / b;
  return true;
}

HEADER_FUNC bool
safe_div_i64 (i64 *d, i64 a, i64 b)
{
  if (b == 0 || (a == INT64_MIN && b == -1)) {
    return false;
  }
  *d = a / b;
  return true;
}

/*-----------------------------------------------------------------------------
 * Floats: succeed only if the result is finite (not inf or NaN)
 *----------------------------------------------------------------------------*/

HEADER_FUNC bool
safe_add_f32 (float *d, float a, float b)
{
  *d = a + b;
  return isfinite (*d) != 0;
}

HEADER_FUNC bool
safe_sub_f32 (float *d, float a, float b)
{
  *d = a - b;
  return isfinite (*d) != 0;
}

HEADER_FUNC bool
safe_mul_f32 (float *d, float a, float b)
{
  *d = a * b;
  return isfinite (*d) != 0;
}

HEADER_FUNC bool
safe_div_f32 (float *d, float a, float b)
{
  if (b == 0.0f) {
    return false;
  }
  *d = a / b;
  return isfinite (*d) != 0;
}

HEADER_FUNC bool
safe_add_f64 (double *d, double a, double b)
{
  *d = a + b;
  return isfinite (*d) != 0;
}

HEADER_FUNC bool
safe_sub_f64 (double *d, double a, double b)
{
  *d = a - b;
  return isfinite (*d) != 0;
}

HEADER_FUNC bool
safe_mul_f64 (double *d, double a, double b)
{
  *d = a * b;
  return isfinite (*d) != 0;
}

HEADER_FUNC bool
safe_div_f64 (double *d, double a, double b)
{
  if (b == 0.0) {
    return false;
  }
  *d = a / b;
  return isfinite (*d) != 0;
}

/*-----------------------------------------------------------------------------
 * Range check: true if [off, off + len) fits inside [0, cap). Never overflows.
 *
 *   if (!bounds_range_ok (src->head, dlen, src->dlen)) { ... }
 *----------------------------------------------------------------------------*/

HEADER_FUNC bool
bounds_range_ok (u64 off, u64 len, u64 cap)
{
  return len <= cap && off <= cap - len;
}

/*-----------------------------------------------------------------------------
 * Error handling
 *----------------------------------------------------------------------------*/

HEADER_FUNC err_t
safe_add_u64_err (u64 *dest, const u64 a, const u64 b, error *e)
{
  if (!safe_add_u64 (dest, a, b)) {
    return error_causef (e, ERR_ARITH, "Overflow");
  }
  return SUCCESS;
}

// dest = dest + arg
HEADER_FUNC err_t
safe_add_u16_err (u16 *dest, u16 arg, error *e)
{
  // Compared in u32 so the check never depends on how UINT16_MAX and the u16
  // operands promote
  if ((u32)*dest + (u32)arg > (u32)UINT16_MAX) {
    return error_causef (
        e,
        ERR_ARITH,
        "Arithmetic overflow: %d + %d > %d\n",
        *dest,
        arg,
        UINT16_MAX
    );
  }
  *dest += arg;
  return SUCCESS;
}

// dest = dest + (arg1 * arg2)
HEADER_FUNC err_t
safe_add_mul_u16_err (u16 *dest, u16 arg1, u16 arg2, error *e)
{
  u32 product = (u32)arg1 * (u32)arg2;
  if (product > (u32)UINT16_MAX) {
    return error_causef (
        e,
        ERR_ARITH,
        "Arithmetic overflow: %d * %d > %d\n",
        arg1,
        arg2,
        UINT16_MAX
    );
  }
  return safe_add_u16_err (dest, product, e);
}

#endif
