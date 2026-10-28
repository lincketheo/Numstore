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
#include "core/ns_platform.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "nscore/testing/simulation/ns_numstore_simulation.h"
#include "nscore/testing/simulation/ns_operation_generator.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Defaults
#define DEFAULT_DBNAME      "ns_simtest"
#define DEFAULT_DURATION    50
#define DEFAULT_SEED        1231241123ULL
#define DEFAULT_COMMIT_HASH "foo"
#define DEFAULT_SEQID       10
#define DEFAULT_FAIL_PCT    0.0f

static _Atomic bool running = true;

static void
exit_handler (int sig)
{
  (void)sig;
  running = false;
}

static void
print_usage (FILE *out, const char *prog)
{
  fprintf (
      out,
      "Usage: %s [options]\n"
      "\n"
      "Options:\n"
      "  --dbname NAME        database name                 (default: %s)\n"
      "  --duration SECONDS   run time in seconds, > 0      (default: %d)\n"
      "  --seed N             RNG seed                      (default: %llu)\n"
      "  --commit-hash HASH   commit hash to record         (default: %s)\n"
      "  --seqid N            sequence id                   (default: %d)\n"
      "  --fail-percent P     fault injection rate in [0,1]  (default: %.4f)\n"
      "  --disable ACTION     disable an action; repeatable\n"
      "  -h, --help           show this help\n"
      "\n"
      "Values can be given as --flag VALUE or --flag=VALUE.\n"
      "\n"
      "The system under test runs on a faulty OS and a faulty allocator that\n"
      "wrap the real ones: every fallible call fails with probability\n"
      "--fail-percent. At 0 they are pure pass-through. Metrics and the\n"
      "reference model always run on the real OS and allocator.\n"
      "\n"
      "Actions (all enabled by default; NSS_ prefix and case optional):\n",
      prog,
      DEFAULT_DBNAME,
      DEFAULT_DURATION,
      DEFAULT_SEED,
      DEFAULT_COMMIT_HASH,
      DEFAULT_SEQID,
      (double)DEFAULT_FAIL_PCT
  );

  for (int a = 0; a < NSS_AT_LEN; ++a) {
    fprintf (out, "  NSS_%s\n", action_names[a] ? action_names[a] : "?");
  }
}

static int
usage_error (const char *prog, const char *fmt, ...)
{
  va_list ap;
  fprintf (stderr, "%s: ", prog);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fprintf (stderr, "\nTry '%s --help' for usage.\n", prog);
  return EXIT_FAILURE;
}

/// Matches "--name VALUE" or "--name=VALUE" at argv[*i].
/// Returns false if argv[*i] is a different argument. On a match, *value is
/// the value (NULL if missing) and *i is advanced past a separate value.
static bool
take_flag (const char *name, int argc, char **argv, int *i, const char **value)
{
  const char *arg = argv[*i];
  size_t      len = strlen (name);

  if (strncmp (arg, name, len) != 0) {
    return false;
  }

  if (arg[len] == '=') {
    *value = arg + len + 1;
  } else if (arg[len] != '\0') {
    return false; // e.g. --seedling is not --seed
  } else if (*i + 1 < argc) {
    *value = argv[++*i];
  } else {
    *value = NULL;
  }

  return true;
}

/// Strict unsigned parse: whole string must be a number in [0, max]
static bool
parse_u64 (const char *s, u64 max, u64 *out)
{
  char              *end;
  unsigned long long v;

  if (*s == '\0' || *s == '-' || *s == '+') {
    return false;
  }

  errno = 0;
  v     = strtoull (s, &end, 10);
  if (errno != 0 || *end != '\0' || v > max) {
    return false;
  }

  *out = (u64)v;
  return true;
}

/// Strict parse of a probability: whole string must be a number in [0, 1]
static bool
parse_prob (const char *s, float *out)
{
  char  *end;
  double v;

  if (*s == '\0') {
    return false;
  }

  errno = 0;
  v     = strtod (s, &end);
  if (errno != 0 || *end != '\0' || !(v >= 0.0 && v <= 1.0)) {
    return false;
  }

  *out = (float)v;
  return true;
}

/// ASCII case-insensitive compare of up to n chars
static bool
ieqn (const char *a, const char *b, size_t n)
{
  for (size_t k = 0; k < n; ++k) {
    if (toupper ((unsigned char)a[k]) != toupper ((unsigned char)b[k])) {
      return false;
    }
    if (a[k] == '\0') {
      return true;
    }
  }
  return true;
}

/// "NSS_BEGIN_TXN", "BEGIN_TXN", "begin_txn" -> NSS_BEGIN_TXN
static bool
parse_action (const char *s, enum ns_action_type *out)
{
  if (ieqn (s, "NSS_", 4)) {
    s += 4;
  }

  for (int a = 0; a < NSS_AT_LEN; ++a) {
    if (action_names[a] && ieqn (s, action_names[a], SIZE_MAX)) {
      *out = (enum ns_action_type)a;
      return true;
    }
  }
  return false;
}

int
main (int argc, char **argv)
{
  error                       e           = error_create ();
  struct ns_simulation_params params      = {0};

  const char                 *dbname      = DEFAULT_DBNAME;
  u64                         duration    = DEFAULT_DURATION;
  u64                         seed        = DEFAULT_SEED;
  const char                 *commit_hash = DEFAULT_COMMIT_HASH;
  u64                         seqid       = DEFAULT_SEQID;
  float                       fail_pct    = DEFAULT_FAIL_PCT;

  // All actions enabled by default
  memset (params.enabled, 1, sizeof (params.enabled));

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    const char *v   = NULL;

    if (strcmp (arg, "-h") == 0 || strcmp (arg, "--help") == 0) {
      print_usage (stdout, argv[0]);
      return EXIT_SUCCESS;
    } else if (take_flag ("--dbname", argc, argv, &i, &v)) {
      if (!v || !*v) {
        return usage_error (argv[0], "%s requires a value", arg);
      }
      dbname = v;
    } else if (take_flag ("--duration", argc, argv, &i, &v)) {
      if (!v || !*v) {
        return usage_error (argv[0], "%s requires a value", arg);
      }
      if (!parse_u64 (v, INT_MAX, &duration) || duration == 0) {
        return usage_error (argv[0], "invalid duration '%s' (must be > 0)", v);
      }
    } else if (take_flag ("--seed", argc, argv, &i, &v)) {
      if (!v || !*v) {
        return usage_error (argv[0], "%s requires a value", arg);
      }
      if (!parse_u64 (v, UINT64_MAX, &seed)) {
        return usage_error (argv[0], "invalid seed '%s'", v);
      }
    } else if (take_flag ("--commit-hash", argc, argv, &i, &v)) {
      if (!v || !*v) {
        return usage_error (argv[0], "%s requires a value", arg);
      }
      commit_hash = v;
    } else if (take_flag ("--seqid", argc, argv, &i, &v)) {
      if (!v || !*v) {
        return usage_error (argv[0], "%s requires a value", arg);
      }
      if (!parse_u64 (v, UINT32_MAX, &seqid)) {
        return usage_error (argv[0], "invalid seqid '%s'", v);
      }
    } else if (take_flag ("--fail-percent", argc, argv, &i, &v)) {
      if (!v || !*v) {
        return usage_error (argv[0], "%s requires a value", arg);
      }
      if (!parse_prob (v, &fail_pct)) {
        return usage_error (argv[0], "invalid fail percent '%s' (must be in [0, 1])", v);
      }
    } else if (take_flag ("--disable", argc, argv, &i, &v)) {
      enum ns_action_type a;

      if (!v || !*v) {
        return usage_error (argv[0], "%s requires a value", arg);
      }
      if (!parse_action (v, &a)) {
        return usage_error (argv[0], "unknown action '%s' (see --help)", v);
      }

      params.enabled[a] = 0;
    } else {
      return usage_error (argv[0], "unknown argument '%s'", arg);
    }
  }

  // At least one action must remain
  bool any_enabled = false;
  for (int a = 0; a < NSS_AT_LEN; ++a) {
    any_enabled = any_enabled || params.enabled[a];
  }

  if (!any_enabled) {
    return usage_error (argv[0], "all actions are disabled");
  }

  // Fault injection is deterministic: randf() is backed by libc rand(), which
  // srand(seed) below pins, so one (seed, fail-percent) pair replays exactly.
  srand ((unsigned)seed);

  // The reliable layer - metrics, the reference model, and the faulty
  // decorators' own bookkeeping all run on this
  const struct i_mem reliable_mem = default_mem ();

  struct i_os        reliable_os;
  if (system_os_create (reliable_mem, &reliable_os, &e)) {
    error_log_consume (&e);
    return EXIT_FAILURE;
  }

  // The layer under test - wraps the reliable one and injects faults
  struct i_os test_os;
  if (faulty_os_create (reliable_mem, reliable_os, fail_pct, &test_os, &e)) {
    error_log_consume (&e);
    system_os_free (reliable_os);
    return EXIT_FAILURE;
  }

  struct i_mem test_mem;
  if (faulty_mem_create (reliable_mem, fail_pct, &test_mem, &e)) {
    error_log_consume (&e);
    faulty_os_free (test_os);
    system_os_free (reliable_os);
    return EXIT_FAILURE;
  }

  params.seed                 = seed;
  params.commit_hash          = commit_hash;
  params.sequence_id          = (u32)seqid; // range-checked above
  params.dbname               = dbname;
  params.max_insert_len       = 1000000;
  params.max_tsize            = 4096;
  params.sample_space_prob    = 0;
  params.reliable_mem         = reliable_mem;
  params.reliable_os          = reliable_os;
  params.test_mem             = test_mem;
  params.test_os              = test_os;

  struct ns_simulation *simul = ns_simul_open (params, &e);
  if (simul == NULL) {
    error_log_consume (&e);
    faulty_mem_free (test_mem);
    faulty_os_free (test_os);
    system_os_free (reliable_os);
    return EXIT_FAILURE;
  }

  i_timer timer;
  if (reliable_os.table->timer_create (reliable_os.self, &timer, &e)) {
    error_log_consume (&e);
    ns_simul_close (simul, &e);
    faulty_mem_free (test_mem);
    faulty_os_free (test_os);
    system_os_free (reliable_os);
    return EXIT_FAILURE;
  }

  // Register SIGINT.
#if PLATFORM_WINDOWS
  if (signal (SIGINT, exit_handler) == SIG_ERR) {
    perror ("signal");
    goto fail;
  }
#else
  struct sigaction sa;
  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = exit_handler;
  sigemptyset (&sa.sa_mask);
  sa.sa_flags = 0;

  if (sigaction (SIGINT, &sa, NULL) == -1) {
    perror ("sigaction");
    goto fail;
  }
#endif

  int rc = EXIT_SUCCESS;

  while (running) {
    if (ns_simul_step (simul, &e) < 0) {
      error_log_consume (&e);
      rc = EXIT_FAILURE;
      break;
    }

    const u64 elapsed_ns = timer.table->timer_now_ns (timer.self);
    if ((f64)elapsed_ns / 1e9 > (f64)duration) {
      running = false;
    }
  }

  ns_simul_close (simul, &e);
  reliable_os.table->timer_free (reliable_os.self, timer.self);
  faulty_mem_free (test_mem);
  faulty_os_free (test_os);
  system_os_free (reliable_os);

  return rc;

fail:
  ns_simul_close (simul, &e);
  reliable_os.table->timer_free (reliable_os.self, timer.self);
  faulty_mem_free (test_mem);
  faulty_os_free (test_os);
  system_os_free (reliable_os);

  return EXIT_FAILURE;
}
