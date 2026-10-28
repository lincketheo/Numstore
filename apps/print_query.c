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

/*
 * query_compile_cli.c
 *
 * Usage:
 *   ./query_compile_cli "<query string>"
 *
 * Compiles the given query string and prints the compiled query to stdout.
 */

#include "core/ns_arena_alloc.h"
#include "core/ns_error.h"
#include "core/ns_string.h"
#include "nscore/compiler/ns_compiler.h"
#include "nscore/types/ns_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_usage (FILE *out, const char *prog)
{
  fprintf (
      out,
      "Usage: %s \"<query string>\"\n"
      "\n"
      "Compiles the given query string with the numstore query compiler and\n"
      "prints the resulting query's type and variable name to stdout.\n"
      "\n"
      "Arguments:\n"
      "  <query string>   a single numstore query, e.g. \"read foo\"\n"
      "\n"
      "Examples:\n"
      "  %s \"read foo\"                       # QT_READ on variable foo\n"
      "  %s \"read foo[0:10:20]\"               # read with a stride\n"
      "  %s \"write foo\"                       # QT_WRITE on variable foo\n"
      "  %s \"insert foo 0 1\"                  # insert 1 elem at offset 0\n"
      "  %s \"remove foo\"                      # QT_REMOVE on variable foo\n"
      "  %s \"create foo u32\"                  # create a u32 variable\n"
      "  %s \"delete foo\"                      # delete variable foo\n"
      "  %s \"get foo\"                         # QT_GET on variable foo\n"
      "  %s \"get if exists foo\"               # QT_GET, no error if missing\n"
      "  %s \"exit\"                            # QT_EXIT\n"
      "  %s \"help\"                            # QT_HELP\n",
      prog,
      prog,
      prog,
      prog,
      prog,
      prog,
      prog,
      prog,
      prog,
      prog,
      prog,
      prog
  );
}

static const char *
query_type_name (enum query_type t)
{
  switch (t) {
    case QT_READ: return "READ";
    case QT_WRITE: return "WRITE";
    case QT_INSERT: return "INSERT";
    case QT_REMOVE: return "REMOVE";
    case QT_CREATE: return "CREATE";
    case QT_DELETE: return "DELETE";
    case QT_GET: return "GET";
    case QT_EXIT: return "EXIT";
    case QT_HELP: return "HELP";
  }
  return "UNKNOWN";
}

int
main (int argc, char **argv)
{
  if (argc == 2 && (strcmp (argv[1], "-h") == 0 || strcmp (argv[1], "--help") == 0)) {
    print_usage (stdout, argv[0]);
    return EXIT_SUCCESS;
  }

  if (argc != 2) {
    print_usage (stderr, argv[0]);
    return EXIT_FAILURE;
  }

  const char *query_str = argv[1];

  ALLOC_INIT (alloc);

  struct query q;
  error        e  = error_create ();
  int          rc = EXIT_SUCCESS;

  if (compile_query (&q, query_str, &alloc, &e)) {
    error_log_consume (&e);
    fprintf (stderr, "error: failed to compile query: %s\n", query_str);
    rc = EXIT_FAILURE;
    goto theend;
  }

  printf ("%s", query_type_name (q.type));

  struct string vname;
  if (!query_vname_of_interest (&vname, &q, &e)) {
    printf (" %.*s", vname.len, vname.data);
  } else {
    error_log_consume (&e);
  }
  printf ("\n");

theend:
  ALLOC_CLOSE (alloc);
  return rc;
}
