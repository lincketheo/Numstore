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
#include "nscore/wal/ns_wal.h"

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
      "Read every record out of the write-ahead log FNAME in order and\n"
      "print a short header summary of each at LOG_INFO. Build with a\n"
      "higher I_LOG_LEVEL (the default is LOG_ERROR) to see the output.\n"
      "\n"
      "Arguments:\n"
      "  FNAME   path to a numstore WAL file (e.g. mydb.db.wal)\n"
      "\n"
      "Examples:\n"
      "  %s mydb.db.wal                # print every WAL record header\n",
      prog,
      prog
  );
}

static void
walf_print (const char *fname)
{
  error       e = error_create ();

  struct i_os os;
  if (system_os_create (default_mem (), &os, &e)) {
    error_log_consume (&e);
    return;
  }

  struct wal *wf = wal_open (fname, default_mem (), os, &e);

  if (wf == NULL) {
    error_log_consume (&e);
    system_os_free (os);
    return;
  }

  while (true) {
    lsn                      rlsn;
    struct wal_rec_hdr_read *out = wal_read_next (wf, &rlsn, &e);

    if (out == NULL) {
      error_log_consume (&e);
      goto theend;
    }

    i_print_wal_rec_hdr_read_light (LOG_INFO, out, rlsn);

    if (out->type == WL_EOF) {
      break;
    }
  }

theend:
  wal_close (wf, &e);
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

  walf_print (fname);

  return 0;
}
