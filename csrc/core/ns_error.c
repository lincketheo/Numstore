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

#include "core/ns_error.h"

#include "core/ns_csx_assert.h"
#include "core/ns_logging.h"
#include "core/ns_testing.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

DEFINE_DBG_ASSERT (error, error, e, {
  ASSERT (e);
  if (e->cause_code != SUCCESS) {
    ASSERT (e->cmlen > 0);
  }
})

error
error_create (void)
{
  const error ret = {
      .cause_code = SUCCESS,
      .cmlen      = 0,
  };

  DBG_ASSERT (error, &ret);

  return ret;
}

void
error_silence (error *e)
{
  e->disable_log = true;
}

void
error_unsilence (error *e)
{
  e->disable_log = false;
}

err_t
error_causef (error *e, const err_t c, const char *fmt, ...)
{
  if (e == NULL) {
    return c;
  }

  ASSERT (fmt);

  va_list ap;
  va_start (ap, fmt);

  char    tmpbuf[256];
  va_list ap_copy;
  va_copy (ap_copy, ap);
  // vsnprintf returns int: negative on an encoding error, and MSVC's CRT is
  // the one that actually does so. Assigning that straight to u32 wrapped it
  // to a huge value, and left tmpbuf possibly unterminated for the %s below.
  const int written = vsnprintf (tmpbuf, sizeof (tmpbuf), fmt, ap_copy);
  va_end (ap_copy);

  u32 cmlen;
  if (written < 0) {
    tmpbuf[0] = '\0'; // vsnprintf gives no termination guarantee on failure
    cmlen     = 0;
  } else {
    cmlen = MIN ((u32)written, 255);
  }

  if (e->cause_code != SUCCESS) {
    if (!e->disable_log) {
      i_log_error ("TRACE: %s\n", tmpbuf);
    }
  }

  if (e->cause_code == SUCCESS) {
    memcpy (e->cause_msg, tmpbuf, cmlen);
    e->cause_code       = c;
    e->cause_msg[cmlen] = '\0';
    e->cmlen            = cmlen;

    if (!e->disable_log) {
      i_log_error ("%.*s\n", e->cmlen, e->cause_msg);
    }
  }

  va_end (ap);

  return c;
}

err_t
error_trace_at (error *e, const char *file, const int line, const char *func)
{
  if (e->cause_code < 0) {
    return error_causef (e, e->cause_code, FPREFIX_STR, file, line, func);
  }
  return e->cause_code;
}

void
error_log_consume (error *e)
{
  DBG_ASSERT (error, e);
  ASSERT (e->cause_code != SUCCESS);
  i_log_error ("%.*s\n", e->cmlen, e->cause_msg);
  e->cmlen      = 0;
  e->cause_code = SUCCESS;
}

#ifndef NDEBUG
TEST (error_log_consume)
{
  TEST_CASE ("error consumes")
  {
    error_causef (&ctx->e, ERR_CORRUPT, "Test");
    test_assert (ctx->e.cause_code == ERR_CORRUPT);
    error_log_consume (&ctx->e);
    test_assert (ctx->e.cause_code == SUCCESS);
    test_assert (ctx->e.cmlen == 0);
  }
}
#endif
