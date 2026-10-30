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
 * @brief Useful error handling with strings
 *
 * The approach to error handling in numstore is to
 * pass around an error struct with a message buffer
 * and assign return codes for error handling results
 */

#ifndef ERROR_H
#define ERROR_H

#include "core/ns_csx_assert.h"
#include "core/ns_platform.h" // PRINTF_ATTR / unlikely
#include "core/ns_stdtypes.h" // u32
#include "core/ns_utils.h"    // FPREFIX_STR

#include <stdbool.h>

#define SUCCESS              0    // Operation completed successfully.
#define ERR_IO               (-1) // Generic I/O error (read, write, or fsync failure).
#define ERR_NOMEM            (-2) // Memory allocation failed.
#define ERR_ARITH            (-3) // Integer arithmetic overflow detected.
#define ERR_CORRUPT          (-4) // Corrupted data - user might've tampered with something
#define ERR_INVALID_ARGUMENT (-5) // User provided an invalid argument
#define ERR_PG_OUT_OF_RANGE  (-6)
#define ERR_SYNTAX           (-7)
#define ERR_INTERP           (-8)
#define ERR_RPTREE_PAGE_STACK_OVERFLOW (-9)
#define ERR_DUPLICATE_VARIABLE         (-10)
#define ERR_VARIABLE_NE                (-11)
#define ERR_DUPLICATE_COMMIT           (-12)
#define ERR_UNHANDLED                  (-13)
#define ERR_TOO_MANY_FILES             (-14)
#define ERR_TOO_MANY_THREADS           (-15)

typedef int err_t;

typedef struct
{
  err_t cause_code;     // Machine-readable error code. @c SUCCESS when no error is
                        // pending.
  char  cause_msg[256]; // Null-terminated human-readable description of the
                        // failure.
  u32   cmlen;          // Length of @c cause_msg in bytes, excluding the null terminator.
  bool  disable_log;    // disable the error log temporarily
} error;

DEFINE_DBG_ASSERT (error, clean_error, e, {
  ASSERT (e);
  ASSERT (e->cause_code == SUCCESS);
  ASSERT (e->cmlen == 0);
})

typedef err_t (*isvalid_func) (void *ctx, error *e);

error error_create (void);
void error_silence (error *e);
void error_unsilence (error *e);
err_t error_causef (error *e, err_t c, const char *fmt, ...) PRINTF_ATTR (3, 4);
void error_log_consume (error *e);

err_t error_trace_at (error *e, const char *file, int line, const char *func);
#define error_trace(e) error_trace_at ((e), FPREFIX_ARGS)

#define WRAP(expr)                     \
  do {                                 \
    if (unlikely ((expr) < SUCCESS)) { \
      return error_trace (e);          \
    }                                  \
  }                                    \
  while (0)

#define WRAP_GOTO(expr, label)         \
  do {                                 \
    if (unlikely ((expr) < SUCCESS)) { \
      goto label;                      \
    }                                  \
  }                                    \
  while (0)

#define CHECK_UNHANDLED_ERROR(e)      \
  do {                                \
    if ((e)->cause_code < 0) {        \
      return error_causef (           \
          (e),                        \
          ERR_UNHANDLED,              \
          "Unhandled error: %d %.*s", \
          (e)->cause_code,            \
          (e)->cmlen,                 \
          (e)->cause_msg              \
      );                              \
    }                                 \
  }                                   \
  while (0)

#define error_reset(e)   \
  do {                   \
    (e)->cause_code = 0; \
    (e)->cmlen      = 0; \
  }                      \
  while (0)

#include <stddef.h>

/**
 * returns the first available non null
 * error
 */
HEADER_FUNC error *
error_priority_n (error *const *errs, size_t n)
{
  for (size_t i = 0; i < n; ++i) {
    if (errs[i] != NULL) {
      return errs[i];
    }
  }
  UNREACHABLE ();
}

// Short hand for error_priority_n
#define error_priority(...) \
  error_priority_n ((error *[]){__VA_ARGS__}, sizeof ((error *[]){__VA_ARGS__}) / sizeof (error *))

#ifdef NDEBUG
#  define FAULT(expr, label) unlikely (expr)
#endif

#endif // ERROR_H
