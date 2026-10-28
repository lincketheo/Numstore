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
 * type_compile_cli.c
 *
 * Usage:
 *   ./type_compile_cli "<type string>"
 *
 * Compiles the given type string and prints the compiled type to stdout.
 */

#include "core/ns_arena_alloc.h"
#include "core/ns_error.h"
#include "nscore/compiler/ns_compiler.h"
#include "nscore/types/ns_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_usage (FILE *out, const char *prog)
{
  fprintf (
      out,
      "Usage: %s \"<type string>\"\n"
      "\n"
      "Compiles the given type string with the numstore type compiler and\n"
      "prints the canonical form of the resulting type and its size in\n"
      "bytes to stdout.\n"
      "\n"
      "Arguments:\n"
      "  <type string>   a single numstore type, e.g. \"u32\"\n"
      "\n"
      "Examples:\n"
      "  %s \"u32\"                                  # a primitive\n"
      "  %s \"[10]u8\"                               # a fixed-size array\n"
      "  %s \"struct { a u32, b f32 }\"              # a struct\n"
      "  %s \"union { a u32, b f32 }\"               # a union\n"
      "  %s \"struct { a struct { b i32 } }\"        # nested types\n",
      prog,
      prog,
      prog,
      prog,
      prog,
      prog
  );
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

  const char *type_str = argv[1];

  ALLOC_INIT (alloc);

  struct type q;
  error       e  = error_create ();
  int         rc = EXIT_SUCCESS;

  if (compile_type (&q, type_str, &alloc, &e)) {
    fprintf (stderr, "error: failed to compile type: %s\n", type_str);
    rc = EXIT_FAILURE;
    goto theend;
  }

  char *str = type_tostr (&alloc, &q, &e);

  if (str == NULL) {
    fprintf (stderr, "error: failed to allocate type string: %s\n", type_str);
    rc = EXIT_FAILURE;
    goto theend;
  }

  fprintf (stdout, "%s\n", str);
  fprintf (stdout, "size: %u bytes\n", type_byte_size (&q));

theend:
  ALLOC_CLOSE (alloc);
  return rc;
}
