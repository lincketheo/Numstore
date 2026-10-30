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

/**
 * @file
 * @brief Anything that has to do with numbers
 *
 * Contains:
 * 1. Checksum utilities
 * 2. Random utilities
 * 3. Numeric Limit checking
 */

#ifndef NUMERICS_H
#define NUMERICS_H

#include "core/ns_crc.h"
#include "core/ns_error.h"
#include "core/ns_platform.h"
#include "core/ns_stdtypes.h"
#include "core/ns_utils.h"

#include <stdbool.h>
#include <stdint.h>

/******************************************************************************
 * SECTION: Random
 ******************************************************************************/

u8 randu8 (void);
u32 randu32 (void);
u64 randu64 (void);

// [lower, upper]
u32 randu32r (u32 lower, u32 upper);
i32 randi32r (i32 lower, i32 upper);
u64 randu64r (u64 lower, u64 upper);
i64 randi64r (i64 lower, i64 upper);
f32 randf (void); // [0, 1]

// [lower, upper)
u64 randu64e (u64 lower, u64 upper);
i64 randi64e (i64 lower, i64 upper);

void rand_bytes (void *dest, u32 len);
#define decl_rand_buffer(name, type, len) \
  type name[len];                         \
  rand_bytes (name, sizeof (type) * (len));

/******************************************************************************
 * SECTION: Parsing and numeric truncation
 ******************************************************************************/

err_t parse_i32_expect (i32 *dest, const char *data, u32 len, error *e);
err_t parse_i64_expect (i64 *dest, const char *data, u32 len, error *e);
err_t parse_f32_expect (f32 *dest, const char *s, u32 len, error *e);
f32 py_mod_f32 (f32 num, f32 denom);
i32 py_mod_i32 (i32 num, i32 denom);

/******************************************************************************
 * SECTION: Math
 ******************************************************************************/

#define i_creal_64(f) (creal (f))
#define i_cimag_64(f) (cimag (f))

#define i_cabs_sqrd_64(f) ((creal (f) * creal (f)) + ((cimag (f) * cimag (f))))
#define i_cabs_64(f)      cabsf (f)
#define i_fabs_32(f)      fabsf (f)

#define arr_range(arr)                       \
  do {                                       \
    for (u32 i = 0; i < arrlen (arr); ++i) { \
      (arr)[i] = i;                          \
    }                                        \
  }                                          \
  while (0)

#define ptr_range(arr, size)              \
  do {                                    \
    for (u32 _i = 0; _i < (size); ++_i) { \
      (arr)[_i] = _i;                     \
    }                                     \
  }                                       \
  while (0)

#define u32_arr_rand(arr)                    \
  do {                                       \
    for (u32 i = 0; i < arrlen (arr); ++i) { \
      (arr)[i] = randu32 ();                 \
    }                                        \
  }                                          \
  while (0)

#define arr_contains(arr, len, val, ret)       \
  do {                                         \
    (ret) = false;                             \
    for (u32 ___i = 0; ___i < (len); ++___i) { \
      if ((arr)[___i] == (val)) {              \
        (ret) = (arr)[___i];                   \
        (ret) = true;                          \
        break;                                 \
      }                                        \
    }                                          \
  }                                            \
  while (0)

float f16_to_f32 (u16 h);

#endif // NUMERICS_H
