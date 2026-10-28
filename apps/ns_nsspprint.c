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

#include "core/ns_logging.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "nscore/disk_pager/ns_file_pager.h"
#include "nscore/page/ns_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_usage (FILE *out, const char *prog)
{
  fprintf (
      out,
      "Usage: %s FNAME\n"
      "\n"
      "Print every page in FNAME at LOG_INFO, reading pages directly off the\n"
      "file pager with no pager-level validation (unlike %s, which goes\n"
      "through the full pager and checks page types). Build with a higher\n"
      "I_LOG_LEVEL (the default is LOG_ERROR) to see the output.\n"
      "\n"
      "Arguments:\n"
      "  FNAME   path to a numstore database file\n"
      "\n"
      "Examples:\n"
      "  %s mydb.db                    # print every raw page in mydb.db\n",
      prog,
      "ns_nspprint",
      prog
  );
}

static void
_simple_page_print (struct file_pager *p, const pgno pg, error *e)
{
  page raw;
  raw.pg = pg;

  if (fpgr_read (p, raw.raw, pg, e)) {
    error_log_consume (e);
    return;
  }

  i_log_page (LOG_INFO, &raw);
}

static void
simple_page_print (const char *fname)
{
  error       e = error_create ();

  struct i_os os;
  if (system_os_create (default_mem (), &os, &e)) {
    error_log_consume (&e);
    return;
  }

  struct file_pager *fp = fpgr_open (fname, default_mem (), os, 0, &e);
  if (fp == NULL) {
    error_log_consume (&e);
    system_os_free (os);
    return;
  }

  for (u32 i = 0; i < fpgr_get_npages (fp); ++i) {
    _simple_page_print (fp, i, &e);
  }

  fpgr_close (fp, &e);
  system_os_free (os);
}

int
main (const int argc, char **argv)
{
  if (argc == 2 && (strcmp (argv[1], "-h") == 0 || strcmp (argv[1], "--help") == 0)) {
    print_usage (stdout, argv[0]);
    return 0;
  }

  if (argc != 2) {
    print_usage (stderr, argv[0]);
    return -1;
  }

  char *fname = argv[1];

  simple_page_print (fname);

  return 0;
}
