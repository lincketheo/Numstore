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
#include "nscore/page/ns_page_fixture.h"
#include "nscore/pager/ns_pager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_usage (FILE *out, const char *prog)
{
  fprintf (
      out,
      "Usage: %s FNAME [PGNO...] [DL|IN]\n"
      "\n"
      "Print page metadata for the pages in FNAME at LOG_INFO. Build with a\n"
      "higher I_LOG_LEVEL (the default is LOG_ERROR) to see the output.\n"
      "\n"
      "Arguments:\n"
      "  FNAME    path to a numstore database file\n"
      "  PGNO...  zero or more page numbers to restrict output to\n"
      "           (default: every page in the file)\n"
      "  DL       only print PG_DATA_LIST pages\n"
      "  IN       only print PG_INNER_NODE pages\n"
      "\n"
      "Examples:\n"
      "  %s mydb.db                    # print every page\n"
      "  %s mydb.db 0 3 7              # print only pages 0, 3 and 7\n"
      "  %s mydb.db DL                 # print only data-list pages\n"
      "  %s mydb.db 0 3 7 IN           # print pages 0, 3, 7 if they are inner nodes\n",
      prog,
      prog,
      prog,
      prog,
      prog
  );
}

typedef struct
{
  u32 *pgnos;
  u32  pglen;
  u32  pgcap;
  int  flag;
} pp_params;

static void
_page_print (struct pager *p, const pgno pg, const pp_params params, error *e)
{
  page_h cur = page_h_create ();

  if (pgr_get (&cur, PG_PERMISSIVE, pg, p, e)) {
    error_log_consume (e);
    return;
  }

  if (params.flag == 0 || (params.flag & page_h_type (&cur))) {
    i_log_page (LOG_INFO, page_h_ro (&cur));
  }

  if (pgr_release (p, &cur, PG_PERMISSIVE, e)) {
    error_log_consume (e);
    abort ();
  }
}

static void
page_print (const char *fname, const pp_params params)
{
  error       e = error_create ();

  struct i_os os;
  if (system_os_create (default_mem (), &os, &e)) {
    error_log_consume (&e);
    return;
  }

  struct pager *p = pgr_open (fname, default_mem (), os, &e);
  if (p == NULL) {
    error_log_consume (&e);
    system_os_free (os);
    return;
  }

  for (u32 i = 0; i < pgr_get_npages (p); ++i) {
    bool contains;
    arr_contains (params.pgnos, params.pglen, i, contains);
    if (params.pglen == 0 || contains) {
      _page_print (p, i, params, &e);
    }
  }

  pgr_close (p, &e);
  system_os_free (os);
}

static int
parse_params (const int argc, char *argv[], pp_params *params)
{
  // Initialize
  params->pgnos = NULL;
  params->pglen = 0;
  params->flag  = 0;

  u32 capacity  = 0;

  // Skip program name (argv[0])
  for (int i = 2; i < argc; i++) {
    char *arg = argv[i];

    // Check for flags
    if (strcmp (arg, "IN") == 0) {
      params->flag |= PG_INNER_NODE;
    } else if (strcmp (arg, "DL") == 0) {
      params->flag |= PG_DATA_LIST;
    } else {
      char     *endptr;
      const u64 val = strtoul (arg, &endptr, 10);

      if (*endptr != '\0') {
        fprintf (
            stderr,
            "Invalid "
            "argument: %s\n",
            arg
        );
        free (params->pgnos);
        return -1;
      }

      if (params->pglen >= capacity) {
        capacity       = capacity == 0 ? 4 : capacity * 2;
        u32 *new_pgnos = realloc (params->pgnos, capacity * sizeof (u32));
        if (!new_pgnos) {
          free (params->pgnos);
          return -1;
        }
        params->pgnos = new_pgnos;
      }

      params->pgnos[params->pglen++] = (u32)val;
    }
  }

  return 0;
}

int
main (const int argc, char **argv)
{
  if (argc == 2 && (strcmp (argv[1], "-h") == 0 || strcmp (argv[1], "--help") == 0)) {
    print_usage (stdout, argv[0]);
    return 0;
  }

  pp_params params;
  if (argc == 1 || parse_params (argc, argv, &params)) {
    print_usage (stderr, argv[0]);
    return -1;
  }

  char *fname = argv[1];

  page_print (fname, params);

  return 0;
}
