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
 *   ./resolve_type_ref "<type string>" "<type ref string>"
 *
 * Example:
 *   ./resolve_type_ref "struct { a u32, b f32 }" "struct { f a.a, g a }"
 */

#include "core/ns_arena_alloc.h"
#include "core/ns_error.h"
#include "nscore/compiler/ns_compiler.h"
#include "nscore/types/ns_type_ref.h"
#include "nscore/types/ns_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_usage (FILE *out, const char *prog)
{
  fprintf (
      out,
      "Usage: %s \"<type string>\" \"<type ref string>\"\n"
      "\n"
      "Compiles <type string> as a base type and <type ref string> as a\n"
      "type ref against it, then prints the resulting resolved type to\n"
      "stdout. A type ref field can reference a field of the base type by\n"
      "dotted path (e.g. \"a.a\") to reuse its type, or reuse the whole base\n"
      "type by name (e.g. \"a\").\n"
      "\n"
      "Arguments:\n"
      "  <type string>       the base type, e.g. \"struct { a u32, b f32 }\"\n"
      "  <type ref string>   a type ref resolved against the base type\n"
      "\n"
      "Examples:\n"
      "  %s \"struct { a u32, b f32 }\" \"struct { f a.a, g a }\"\n"
      "      -> struct { f u32, g struct { a u32, b f32 } }\n"
      "\n"
      "  %s \"struct { a u8, b [10][20]u16 }\" \"a.b\"\n"
      "      -> [10][20]u16\n",
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

  if (argc != 3) {
    print_usage (stderr, argv[0]);
    return EXIT_FAILURE;
  }

  const char *type_str     = argv[1];
  const char *type_ref_str = argv[2];

  ALLOC_INIT (alloc);

  struct type     q;
  struct type_ref ref;
  error           e  = error_create ();
  int             rc = EXIT_SUCCESS;

  if (compile_type (&q, type_str, &alloc, &e)) {
    error_log_consume (&e);
    rc = EXIT_FAILURE;
    goto theend;
  }

  if (compile_type_ref (&ref, type_ref_str, &alloc, &e)) {
    error_log_consume (&e);
    rc = EXIT_FAILURE;
    goto theend;
  }

  struct type *generated = tr_construct (&q, &ref, &alloc, &e);
  if (generated == NULL) {
    error_log_consume (&e);
    rc = EXIT_FAILURE;
    goto theend;
  }

  char *str = type_tostr (&alloc, generated, &e);
  if (str == NULL) {
    fprintf (stderr, "error: failed to allocate type string: %s\n", type_str);
    rc = EXIT_FAILURE;
    goto theend;
  }

  fprintf (stdout, "%s\n", str);

theend:
  ALLOC_CLOSE (alloc);
  return rc;
}
