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
      "Usage: %s FNAME PGNO\n"
      "\n"
      "Walk the PG_DATA_LIST / PG_INNER_NODE page chain in FNAME starting at\n"
      "PGNO, writing the raw bytes stored in each data-list page to stdout.\n"
      "Per-page metadata (pgno, next/prev links, byte length) is traced to\n"
      "stderr as each page is visited.\n"
      "\n"
      "Arguments:\n"
      "  FNAME   path to a numstore database file\n"
      "  PGNO    page number to start reading from\n"
      "\n"
      "Examples:\n"
      "  %s mydb.db 3                  # dump the chain starting at page 3\n"
      "  %s mydb.db 3 > out.bin        # save the recovered bytes to a file\n"
      "  %s mydb.db 3 2>/dev/null      # suppress the per-page trace\n",
      prog,
      prog,
      prog,
      prog
  );
}

MAYBE_UNUSED static void
dl_contents_one_page (FILE *out, const page_h *cur)
{
  fprintf (stderr, "============================= %" PRpgno "\n", page_h_pgno (cur));
  fprintf (stderr, "DATA_LIST\n");
  fprintf (stderr, "next: %" PRpgno "\n", dl_get_next (page_h_ro (cur)));
  fprintf (stderr, "prev: %" PRpgno "\n", dl_get_prev (page_h_ro (cur)));
  fprintf (stderr, "blen: %" PRp_size "\n", dl_used (page_h_ro (cur)));

  u8           output[DL_DATA_SIZE];
  const p_size read  = dl_read (page_h_ro (cur), output, 0, DL_DATA_SIZE);

  size_t       total = 0;
  while (total < read) {
    const size_t written = fwrite (output + total, 1, read - total, out);
    if (written == 0) {
      if (ferror (out)) {
        perror ("fwrite");
        break;
      }
      break;
    }
    total += written;
  }

  fprintf (stderr, "Wrote: %zu\n", total);
}

static void
dl_contents (MAYBE_UNUSED FILE *out, const char *fname, MAYBE_UNUSED const pgno pg)
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

  // TODO: walk the chain from [pg] and print it to [out] with
  // dl_contents_one_page.
  if (pgr_close (p, &e)) {
    error_log_consume (&e);
  }
  system_os_free (os);
}

int
main (const int argc, char **argv)
{
  if (argc == 2 && (strcmp (argv[1], "-h") == 0 || strcmp (argv[1], "--help") == 0)) {
    print_usage (stdout, argv[0]);
    return 0;
  }

  if (argc != 3) {
    print_usage (stderr, argv[0]);
    return -1;
  }

  int   pg    = atoi (argv[2]);
  char *fname = argv[1];

  dl_contents (stdout, fname, pg);

  return 0;
}
