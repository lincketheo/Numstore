C API Reference
===============

Numstore is a C11 library with one public header, `include/numstore.h`, and one
static archive, `libnumstore.a`. There are no dependencies beyond libc, pthreads
and libm.

If you are new to Numstore, read [the tour](index.md) first - it explains the
query language and the type system this page assumes. It is written in Python,
but the queries are identical; only the host language changes. For the Python
bindings themselves, see [the Python API](python_api.md).

Every program on this page is complete and compiles as shown, and every output
block is what it actually printed.

* [Build and link](#build-and-link)
* [Quick start](#quick-start)
* [Error handling](#error-handling)
* [Transactions](#transactions)
* [Durability and crash recovery](#durability-and-crash-recovery)
* [Reading into a buffer you own](#reading-into-a-buffer-you-own)
* [Reading into a buffer Numstore allocates](#reading-into-a-buffer-numstore-allocates)
* [Structs and arrays](#structs-and-arrays)
* [Variable metadata](#variable-metadata)
* [Prepared plans](#prepared-plans)
* [Smart files](#smart-files)
* [Concurrency and threading](#concurrency-and-threading)
* [Function reference](#function-reference)
* [Types and constants](#types-and-constants)

Build and link
--------------

Build the library out of the repository:

```sh
git clone https://github.com/NumstoreDB/Numstore
cd Numstore
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

That leaves you with `build/release/lib/libnumstore.a` and the samples in
`build/release/bin/`. To compile a program of your own against it:

```sh
cc -std=c11 -I include myprog.c build/release/lib/libnumstore.a -o myprog
```

On Linux add `-pthread -lm`; on macOS and Windows neither is needed. For a
debug build, use `-B build/debug -DCMAKE_BUILD_TYPE=Debug` instead.

From CMake, point at the checkout and link the exported target. The include
directory comes with it:

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp C)

add_subdirectory(Numstore)

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE numstore::numstore)
```

`cmake --install build/release --prefix /usr/local` installs the archive and the
header if you would rather link against a system path.

Quick start
-----------

Open a database, create a variable, insert data, read it back. A Numstore
database is a single file and there is no server to start:

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>

int
main (void)
{
  ns_cleanup ("prices.db");

  nsdb_t *db = ns_open ("prices.db");
  if (db == NULL) {
    fprintf (stderr, "could not open prices.db\n");
    return 1;
  }

  if (ns_execute (db, NULL, "create prices f64") != 0) {
    fprintf (stderr, "create failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }

  const double in[] = {1.5, 2.25, 3.75};
  sb_size      wrote = ns_write (db, NULL, in, sizeof (in), "insert prices 0 3");
  if (wrote < 0) {
    fprintf (stderr, "insert failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }
  printf ("inserted %" PRId64 " elements\n", wrote);

  double  out[8];
  sb_size read = ns_read (db, NULL, out, sizeof (out), "read prices[0:]");
  if (read < 0) {
    fprintf (stderr, "read failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }

  printf ("read %" PRId64 " elements:", read);
  for (sb_size i = 0; i < read; i++) {
    printf (" %g", out[i]);
  }
  printf ("\n");

  return ns_close (db);
}
```

```text
inserted 3 elements
read 3 elements: 1.5 2.25 3.75
```

Three things carry across every example below:

* **The `NULL` second argument is the transaction.** Pass `NULL` and the query
  gets an automatic transaction of its own - atomic and durable by the time the
  call returns. See [Transactions](#transactions) to group queries instead.
* **Queries are `printf` format strings.** `ns_execute`, `ns_read`, `ns_write`,
  `ns_get_var` and `ns_plan_fcreate` are variadic, so an offset or a count can
  come from a variable:
  `ns_write (db, NULL, src, sizeof (src), "insert prices %d %d", offset, count)`.
* **Buffer sizes are bytes, query counts are elements.** `sizeof (out)` tells
  Numstore how much room it has; `read prices[0:]` says which elements you want.
  The return value is the number of **elements** moved, and reads stop when
  either the range or the buffer runs out.

Which function you call depends on what has to cross the boundary - nothing,
metadata, data in, or data out:

| Function                                | Queries it accepts         | Returns                       |
|-----------------------------------------|----------------------------|-------------------------------|
| `ns_execute (db, tx, fmt, ...)`         | `CREATE` `DELETE` `REMOVE` | `int` - `0` on success        |
| `ns_get_var (db, tx, fmt, ...)`         | anything naming a variable | `nsdb_var_t *`, `NULL` on failure |
| `ns_write (db, tx, src, slen, fmt, ...)`| `INSERT` `WRITE`           | `sb_size` - elements written, `< 0` on failure |
| `ns_read (db, tx, dest, dlen, fmt, ...)`| `READ` `REMOVE`            | `sb_size` - elements read, `< 0` on failure |
| `ns_malloc (db, tx, &dlen, fmt, ...)`   | `READ` `REMOVE`            | `void *`, `NULL` on failure   |

Error handling
--------------

Numstore never aborts on a bad query. Functions returning a pointer return
`NULL`, functions returning `int` return non-zero, and functions returning
`sb_size` return a negative number. `ns_strerror` then describes what happened:

```c
#include "numstore.h"

#include <stdio.h>

int
main (void)
{
  // ns_open_with_error copies its message without a terminator, so zero the
  // buffer. MAX_ERRSTR_LEN is the longest message Numstore produces.
  char    errstr[MAX_ERRSTR_LEN] = {0};
  nsdb_t *db = ns_open_with_error ("no/such/dir/x.db", errstr, sizeof (errstr));
  if (db == NULL) {
    printf ("open failed: %s\n", errstr);
  }

  ns_cleanup ("errors.db");
  db = ns_open ("errors.db");

  if (ns_execute (db, NULL, "not a real query") != 0) {
    printf ("execute failed: %s\n", ns_strerror (db));
  }

  if (ns_execute (db, NULL, "create bad q99") != 0) {
    printf ("bad type: %s\n", ns_strerror (db));
  }

  ns_execute (db, NULL, "create foo u32");
  if (ns_execute (db, NULL, "insert foo 0 10") != 0) {
    printf ("wrong entry point: %s\n", ns_strerror (db));
  }

  double  dest[4];
  sb_size n = ns_read (db, NULL, dest, sizeof (dest), "get foo");
  if (n < 0) {
    printf ("read of a get: %s\n", ns_strerror (db));
  }

  return ns_close (db);
}
```

```text
open failed: open_file_rw no/such/dir/x.db: No such file or directory
execute failed: Expected a valid operation at pos: 0
bad type: Expected type at position 2, got token type TT_IDENTIFIER
wrong entry point: Only supported exec commands are REMOVE/CREATE/DELETE
read of a get: Only supported read commands are READ/REMOVE
```

`ns_open` gives you no message when it fails, because there is no database
handle yet to ask. Use `ns_open_with_error` when you need to report why, and
zero the buffer first - the message is copied into it without a terminator.

A failed query leaves no partial work behind - a query is atomic whether or not
you handed it a transaction - and the handle stays usable afterwards, so you can
report the error and carry on. Plans report through `ns_plan_strerror (plan)`
rather than `ns_strerror (db)`.

Transactions
------------

`ns_begin` opens a transaction; pass it where you were passing `NULL` and the
queries become one atomic unit. `ns_commit` makes it durable, `ns_rollback`
throws it away, and reads inside the transaction see its own uncommitted work:

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>

static void
print_prices (nsdb_t *db, const char *label)
{
  double  out[8];
  sb_size n = ns_read (db, NULL, out, sizeof (out), "read prices[0:]");
  printf ("%s:", label);
  for (sb_size i = 0; i < n; i++) {
    printf (" %g", out[i]);
  }
  printf ("\n");
}

int
main (void)
{
  ns_cleanup ("txn.db");
  nsdb_t *db = ns_open ("txn.db");
  ns_execute (db, NULL, "create prices f64");

  // Commit: both inserts land together, or neither does.
  txn_t *tx = ns_begin (db);
  if (tx == NULL) {
    fprintf (stderr, "begin failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }

  const double first[] = {1.5, 2.25};
  const double second[] = {3.75};
  ns_write (db, tx, first, sizeof (first), "insert prices 0 2");
  ns_write (db, tx, second, sizeof (second), "insert prices 2 1");

  if (ns_commit (db, tx) != 0) {
    fprintf (stderr, "commit failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }
  print_prices (db, "after commit ");

  // Rollback: the insert is thrown away.
  tx = ns_begin (db);
  const double third[] = {99.0};
  ns_write (db, tx, third, sizeof (third), "insert prices 0 1");

  double  inside[8];
  sb_size n = ns_read (db, tx, inside, sizeof (inside), "read prices[0:]");
  printf ("inside txn  : %" PRId64 " elements, first %g\n", n, inside[0]);

  ns_rollback (db, tx);
  print_prices (db, "after rollback");

  return ns_close (db);
}
```

```text
after commit : 1.5 2.25 3.75
inside txn  : 4 elements, first 99
after rollback: 1.5 2.25 3.75
```

A `txn_t *` is spent once you commit or roll it back - don't reuse it. Finish
every transaction you open: `ns_close` blocks while one is still open, and
`ns_begin` blocks while another is still open. See
[Concurrency and threading](#concurrency-and-threading).

Durability and crash recovery
-----------------------------

Numstore writes ahead to a log and recovers with ARIES on the next open: work
from a committed transaction is redone, work from a transaction that never
committed is undone. A commit is durable the moment `ns_commit` returns, whether
or not you get to call `ns_close`.

This program proves it by crashing on purpose. Each phase runs in a forked child
that calls `_Exit (1)` instead of `ns_close`, which gives the library no chance
to flush or clean up:

```c
#include "numstore.h"

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH "durability.db"

// Commit, then die without closing: ARIES redoes the committed work on reopen.
static void
commit_then_crash (void)
{
  nsdb_t *db = ns_open (PATH);
  txn_t  *tx = ns_begin (db);
  ns_write (db, tx, "BB", 2, "insert log 3 2");
  ns_commit (db, tx);
  _Exit (1); // no ns_close
}

// Die mid transaction: ARIES undoes the uncommitted work on reopen.
static void
crash_without_commit (void)
{
  nsdb_t *db = ns_open (PATH);
  txn_t  *tx = ns_begin (db);
  ns_write (db, tx, "CC", 2, "insert log 7 2");
  _Exit (1); // no ns_commit, no ns_close
}

static void
in_child (void (*phase) (void))
{
  pid_t pid = fork ();
  if (pid == 0) {
    phase ();
    _Exit (0);
  }
  waitpid (pid, NULL, 0);
}

int
main (void)
{
  ns_cleanup (PATH);

  nsdb_t *db = ns_open (PATH);
  ns_execute (db, NULL, "create log u8");
  ns_write (db, NULL, "AAAAAAAAAA", 10, "insert log 0 10");
  ns_close (db);

  in_child (commit_then_crash);
  in_child (crash_without_commit);

  // Reopening replays the log: committed work is there, the rest is gone.
  db = ns_open (PATH);
  char    out[16] = {0};
  sb_size n = ns_read (db, NULL, out, sizeof (out) - 1, "read log[0:]");
  printf ("after recovery: %.*s\n", (int)n, out);
  return ns_close (db);
}
```

```text
after recovery: AAABBAAAAAAA
```

The committed `BB` is at offset 3 where the first child put it. The `CC` from
the child that never committed is gone, and the array is 12 elements rather than
14. There is no state in between: half a transaction is never visible.

`ns_crash (db)` does this deliberately from inside a live process - it drops the
handle without flushing, leaving any open transaction to be rolled back on the
next open. It is how the test suite exercises recovery; use it for the same thing
rather than as a fast path for closing.

Two longer samples ship with the repository:
[`samples/smfile_sample4_rollback_commit.c`](../samples/smfile_sample4_rollback_commit.c)
walks all five commit/crash/rollback combinations, and
[`samples/sample4_crash.py`](../samples/sample4_crash.py) is the same idea from
Python.

Reading into a buffer you own
-----------------------------

`ns_read` fills a buffer you already have. It never allocates, which makes it
the function for reading a large variable in fixed size chunks - the buffer size
caps each read, and the return value tells you how many elements arrived:

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>

int
main (void)
{
  ns_cleanup ("chunks.db");
  nsdb_t *db = ns_open ("chunks.db");
  ns_execute (db, NULL, "create stream u32");

  uint32_t src[100];
  for (int i = 0; i < 100; i++) {
    src[i] = (uint32_t)i;
  }
  ns_write (db, NULL, src, sizeof (src), "insert stream 0 100");

  // Walk the whole variable 16 elements at a time.
  uint32_t chunk[16];
  b_size   start = 0;
  b_size   total = 0;
  for (;;) {
    sb_size n = ns_read (
        db,
        NULL,
        chunk,
        sizeof (chunk),
        "read stream[%" PRIu64 ":%" PRIu64 "]",
        start,
        start + 16
    );
    if (n <= 0) {
      break;
    }
    total += (b_size)n;
    start += (b_size)n;
  }

  printf ("read %" PRIu64 " elements in chunks of 16\n", total);
  return ns_close (db);
}
```

```text
read 100 elements in chunks of 16
```

Reading into a buffer Numstore allocates
----------------------------------------

`ns_malloc` is `ns_read` without the buffer: it allocates one the right size,
fills it, and writes the **byte** count through `dlen`. The buffer comes from
the system allocator, so release it with plain `free`:

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int
main (void)
{
  ns_cleanup ("malloc.db");
  nsdb_t *db = ns_open ("malloc.db");
  ns_execute (db, NULL, "create r u32");

  const uint32_t src[] = {1, 2, 3, 4, 5, 6};
  ns_write (db, NULL, src, sizeof (src), "insert r 0 6");

  b_size    nbytes = 0;
  uint32_t *data = ns_malloc (db, NULL, &nbytes, "read r[0:6:2]");
  if (data == NULL) {
    fprintf (stderr, "read failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }

  printf ("%" PRIu64 " bytes, %" PRIu64 " elements:", nbytes, nbytes / sizeof (uint32_t));
  for (b_size i = 0; i < nbytes / sizeof (uint32_t); i++) {
    printf (" %u", data[i]);
  }
  printf ("\n");

  free (data); // system free, not an ns_ function
  return ns_close (db);
}
```

```text
12 bytes, 3 elements: 1 3 5
```

Structs and arrays
------------------

Numstore types have no padding: a struct is the sum of its members and a strict
array is the product of its dimensions. A C struct you read into must therefore
be packed, or the compiler's padding will shift every field.

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>

// Numstore types have no padding, so the C struct must have none either.
#ifdef _MSC_VER
#  define PACK_PUSH __pragma (pack (push, 1))
#  define PACK_POP  __pragma (pack (pop))
#  define PACKED
#else
#  define PACK_PUSH
#  define PACK_POP
#  define PACKED __attribute__ ((packed))
#endif

PACK_PUSH
struct frame
{
  uint64_t timestamp;
  uint8_t  pixels[4][4];
} PACKED;
PACK_POP

int
main (void)
{
  ns_cleanup ("frames.db");
  nsdb_t *db = ns_open ("frames.db");

  if (ns_execute (db, NULL, "create frames struct { timestamp u64, pixels [4][4] u8 }") != 0) {
    fprintf (stderr, "create failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }

  printf ("sizeof (struct frame) = %zu\n", sizeof (struct frame));

  struct frame in[2] = {0};
  for (int f = 0; f < 2; f++) {
    in[f].timestamp = 1000 + f;
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        in[f].pixels[r][c] = (uint8_t)(f * 16 + r * 4 + c);
      }
    }
  }

  sb_size wrote = ns_write (db, NULL, in, sizeof (in), "insert frames 0 2");
  printf ("wrote %" PRId64 " frames\n", wrote);

  struct frame out[2] = {0};
  sb_size      read = ns_read (db, NULL, out, sizeof (out), "read frames[0:]");
  printf ("read %" PRId64 " frames\n", read);
  for (sb_size i = 0; i < read; i++) {
    printf (
        "frame %" PRId64 ": timestamp=%" PRIu64 " pixels[0][0]=%u pixels[3][3]=%u\n",
        i,
        out[i].timestamp,
        out[i].pixels[0][0],
        out[i].pixels[3][3]
    );
  }

  nsdb_var_t *var = ns_get_var (db, NULL, "get frames");
  printf ("variable holds %" PRIu64 " elements\n", ns_var_len (var));
  ns_var_free (var);

  return ns_close (db);
}
```

```text
sizeof (struct frame) = 24
wrote 2 frames
read 2 frames
frame 0: timestamp=1000 pixels[0][0]=0 pixels[3][3]=15
frame 1: timestamp=1001 pixels[0][0]=16 pixels[3][3]=31
variable holds 2 elements
```

`8 + 4 x 4 = 24` bytes, which is what both Numstore and the packed struct agree
on. Arrays are stored in C order, so `pixels[r][c]` lines up element for element.
The sizing rules for every type are in
[the type system](index.md#the-numstore-type-system).

Variable metadata
-----------------

`ns_get_var` hands you a handle to one variable's metadata. Any query that names
a variable works and the query itself never runs, so you can ask what a
destructive query *would* touch. Free the handle with `ns_var_free`;
`ns_visit_nsdb_vars` walks every variable in the database instead:

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>

struct total
{
  unsigned seen;
  b_size   elements;
};

static void
tally (nsdb_var_t *var, void *ctx)
{
  struct total *t = ctx;
  t->seen++;
  t->elements += ns_var_len (var);
  // Do not call ns_var_free here: the visitor owns this handle.
}

int
main (void)
{
  ns_cleanup ("vars.db");
  nsdb_t *db = ns_open ("vars.db");

  ns_execute (db, NULL, "create v1 i32");
  ns_execute (db, NULL, "create v2 f64");

  const int32_t src[] = {1, 2, 3};
  ns_write (db, NULL, src, sizeof (src), "insert v1 0 3");

  // One variable at a time.
  nsdb_var_t *var = ns_get_var (db, NULL, "get v1");
  if (var == NULL) {
    fprintf (stderr, "get failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }
  printf ("v1 has %" PRIu64 " elements\n", ns_var_len (var));
  ns_var_free (var);

  // Any query that names a variable works, and the query never runs.
  var = ns_get_var (db, NULL, "remove v1[0:]");
  printf ("v1 still has %" PRIu64 " elements\n", ns_var_len (var));
  ns_var_free (var);

  // Every variable in the database.
  struct total t = {0};
  if (ns_visit_nsdb_vars (db, tally, NULL, &t) != 0) {
    fprintf (stderr, "visit failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }
  printf ("%u variables, %" PRIu64 " elements total\n", t.seen, t.elements);

  return ns_close (db);
}
```

```text
v1 has 3 elements
v1 still has 3 elements
2 variables, 3 elements total
```

Two ownership rules worth keeping straight:

* A handle from `ns_get_var` is yours, and `ns_var_free` releases it. Variables
  get their own memory, so you may free them before or after `ns_close`.
* A handle passed to a `ns_visit_nsdb_vars` callback is **not** yours. Don't
  free it, and don't keep it past the callback - copy out what you need.

`ns_visit_nsdb_vars` takes a transaction argument for symmetry but ignores it
today: the walk always sees committed state.

In C, `ns_var_len` is the whole metadata interface - length in elements. The
type and element size are not exposed as C accessors; when you need those, read
them from the type you wrote in the `create` query, or use
[`Var` in Python](python_api.md#var).

Prepared plans
--------------

`ns_execute`, `ns_read` and `ns_write` compile the query string, run it, and
throw the compiled form away. A plan keeps it, so a query you run in a loop is
compiled once:

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>

int
main (void)
{
  ns_cleanup ("plans.db");
  nsdb_t *db = ns_open ("plans.db");
  ns_execute (db, NULL, "create samples u32");

  // Compile once.
  nsdb_plan_t *append = ns_plan_fcreate (db, "insert samples 0 4");
  if (append == NULL) {
    fprintf (stderr, "plan failed: %s\n", ns_strerror (db));
    ns_close (db);
    return 1;
  }

  // Run it many times.
  for (int batch = 0; batch < 3; batch++) {
    const uint32_t src[] = {1, 2, 3, 4};
    sb_size        n = ns_plan_write (append, NULL, src, sizeof (src));
    if (n < 0) {
      fprintf (stderr, "write failed: %s\n", ns_plan_strerror (append));
      ns_plan_free (append);
      ns_close (db);
      return 1;
    }
    printf ("batch %d wrote %" PRId64 "\n", batch, n);
  }

  nsdb_var_t *var = ns_plan_get_var (append, NULL);
  printf ("samples now holds %" PRIu64 " elements\n", ns_var_len (var));
  ns_var_free (var);

  ns_plan_free (append); // plans must be freed before the database closes

  nsdb_plan_t *head = ns_plan_fcreate (db, "read samples[0:4]");
  uint32_t     out[4];
  sb_size      n = ns_plan_read (head, NULL, out, sizeof (out));
  printf ("first %" PRId64 ":", n);
  for (sb_size i = 0; i < n; i++) {
    printf (" %u", out[i]);
  }
  printf ("\n");
  ns_plan_free (head);

  return ns_close (db);
}
```

```text
batch 0 wrote 4
batch 1 wrote 4
batch 2 wrote 4
samples now holds 12 elements
first 4: 1 2 3 4
```

Each plan function mirrors the one-shot function, with the transaction as an
argument so one compiled plan can run inside any transaction:

| One shot        | Prepared              |
|-----------------|-----------------------|
| `ns_execute`    | `ns_plan_execute`     |
| `ns_read`       | `ns_plan_read`        |
| `ns_write`      | `ns_plan_write`       |
| `ns_malloc`     | `ns_plan_malloc`      |
| `ns_get_var`    | `ns_plan_get_var`     |
| `ns_strerror`   | `ns_plan_strerror`    |

Plans must be freed before the database closes.

Smart files
-----------

A smart file is the same storage engine without the type system or variables:
one untyped byte array in a file, with the same ACID guarantees. Reach for it
when you want transactional inserts into the middle of a blob and don't need a
schema.

Open it with `ns_smfile_open` and address it with byte offsets rather than
queries. `size` is the width of one element and `stride` is how many elements to
step between touches, so a strided write spreads data out without a query
language:

```c
#include "numstore.h"

#include <inttypes.h>
#include <stdio.h>

static void
dump (nsdb_t *smf, const char *label)
{
  uint32_t out[8] = {0};
  sb_size  n = ns_smfile_read (smf, NULL, out, sizeof (uint32_t), 0, 1, 8);
  printf ("%-16s (%" PRId64 "):", label, n);
  for (sb_size i = 0; i < n; i++) {
    printf (" %u", out[i]);
  }
  printf ("\n");
}

int
main (void)
{
  ns_cleanup ("stream.smf");

  nsdb_t *smf = ns_smfile_open ("stream.smf");
  if (smf == NULL) {
    fprintf (stderr, "could not open stream.smf\n");
    return 1;
  }

  // A smart file is one untyped byte array. Offsets and lengths are bytes.
  const uint32_t src[] = {1, 2, 3, 4, 5, 6};
  sb_size        n = ns_smfile_insert (smf, NULL, src, 0, sizeof (src));
  printf ("inserted %" PRId64 " bytes, file is %" PRId64 " bytes\n", n, ns_smfile_size (smf, NULL));
  dump (smf, "after insert");

  // Overwrite every other u32 starting at byte 0.
  const uint32_t patch[] = {70, 80, 90};
  ns_smfile_write (smf, NULL, patch, sizeof (uint32_t), 0, 2, 3);
  dump (smf, "after write");

  // Read every other u32.
  uint32_t strided[3] = {0};
  ns_smfile_read (smf, NULL, strided, sizeof (uint32_t), 0, 2, 3);
  printf ("strided read   : %u %u %u\n", strided[0], strided[1], strided[2]);

  // Remove the first two u32s and keep what came out.
  uint32_t taken[2] = {0};
  ns_smfile_remove (smf, NULL, taken, sizeof (uint32_t), 0, 1, 2);
  printf ("removed        : %u %u\n", taken[0], taken[1]);
  dump (smf, "after remove");

  return ns_close (smf);
}
```

```text
inserted 24 bytes, file is 24 bytes
after insert     (6): 1 2 3 4 5 6
after write      (6): 70 2 80 4 90 6
strided read   : 70 80 90
removed        : 70 2
after remove     (4): 80 4 90 6
```

Smart files share the rest of the API: `ns_begin`, `ns_commit`, `ns_rollback`,
`ns_close`, `ns_crash` and `ns_cleanup` all work on an `nsdb_t *` from
`ns_smfile_open`. What they don't share is the query language - a smart file has
no variables, so `ns_execute`, `ns_read`, `ns_write` and plans don't apply.

One sharp edge: unlike `ns_read`, the smart file functions take **no destination
length**. `nelem` is the only thing bounding the write into `dest`, so it is on
you to keep `size * nelem` within your buffer:

```c
uint32_t out[8];

// Safe: nelem matches the buffer.
ns_smfile_read (smf, NULL, out, sizeof (uint32_t), 0, 1, 8);

// Overflows out if the file holds more than 8 elements - NS_END means
// "to the end of the file", and the call does not know how big out is.
ns_smfile_read (smf, NULL, out, sizeof (uint32_t), 0, 1, NS_END);
```

Use `NS_END` only when the buffer can hold the whole file, which you can check
first with `ns_smfile_size`. Otherwise pass an explicit `nelem` and loop.

Concurrency and threading
-------------------------

Numstore serializes transactions strictly: **one transaction is open at a time
per handle**. `ns_begin` blocks until the open one commits or rolls back, which
is what makes transactions strictly serializable - the schedule is the order
they commit in, with no interleaving to reason about.

What that means in practice:

* Finish each transaction before opening the next one on the same handle. Call
  `ns_begin` twice without committing and the second call blocks forever on a
  transaction only you can finish.
* `ns_close` blocks while a transaction is open, so commit or roll back first.
* Don't open the same file twice, in one process or across processes. Both calls
  succeed today, but the second handle does not see the first handle's commits.
  Treat a database file as owned by one handle at a time.

Concurrent readers and writers are in development. Until they land, the
supported shape is one handle per file with one transaction in flight at a time;
a crash at any point is still safe, which is what
[recovery](#durability-and-crash-recovery) is for.

Function reference
------------------

### Lifecycle

| Function | Returns | Notes |
|----------|---------|-------|
| `nsdb_t *ns_open (const char *path)` | handle, `NULL` on failure | opens or creates a database |
| `nsdb_t *ns_open_with_error (const char *path, char erstr[], unsigned elen)` | handle, `NULL` on failure | same, and copies the failure message into `erstr`; zero the buffer first |
| `nsdb_t *ns_smfile_open (const char *path)` | handle, `NULL` on failure | opens or creates a smart file |
| `nsdb_t *ns_smfile_open_with_error (const char *path, char erstr[], unsigned elen)` | handle, `NULL` on failure | same, with a message |
| `int ns_close (nsdb_t *ns)` | `0` on success | flushes and closes; blocks while a transaction is open |
| `int ns_crash (nsdb_t *ns)` | `0` on success | drops the handle without flushing; open transactions are rolled back on the next open |
| `int ns_cleanup (const char *path)` | `0` on success | deletes the database or smart file at `path` |

### Queries

| Function | Returns | Notes |
|----------|---------|-------|
| `int ns_execute (nsdb_t *db, txn_t *tx, const char *fmt, ...)` | `0` on success | `CREATE` `DELETE` `REMOVE` |
| `sb_size ns_read (nsdb_t *db, txn_t *tx, void *dest, b_size dlen, const char *fmt, ...)` | elements read, `< 0` on failure | `READ` `REMOVE`; `dlen` is bytes |
| `sb_size ns_write (nsdb_t *db, txn_t *tx, const void *src, b_size slen, const char *fmt, ...)` | elements written, `< 0` on failure | `INSERT` `WRITE`; `slen` is bytes |
| `void *ns_malloc (nsdb_t *db, txn_t *tx, b_size *dlen, const char *fmt, ...)` | buffer, `NULL` on failure | `READ` `REMOVE`; writes the byte count to `dlen`; release with `free` |
| `nsdb_var_t *ns_get_var (nsdb_t *db, txn_t *tx, const char *query, ...)` | handle, `NULL` on failure | any query naming a variable; the query never runs |

### Transactions

| Function | Returns | Notes |
|----------|---------|-------|
| `txn_t *ns_begin (nsdb_t *ns)` | handle, `NULL` on failure | blocks while another transaction is open |
| `int ns_commit (nsdb_t *ns, txn_t *txn)` | `0` on success | durable when it returns; spends the handle |
| `int ns_rollback (nsdb_t *ns, txn_t *txn)` | `0` on success | undoes everything; spends the handle |

### Variables

| Function | Returns | Notes |
|----------|---------|-------|
| `b_size ns_var_len (nsdb_var_t *var)` | elements | length when the handle was taken |
| `void ns_var_free (nsdb_var_t *var)` | | frees a handle from `ns_get_var`; may run after `ns_close` |
| `int ns_visit_nsdb_vars (nsdb_t *db, nsdb_var_consumer consumer, txn_t *tx, void *ctx)` | `0` on success | calls `consumer (var, ctx)` per variable; the callback must not free `var`; `tx` is ignored |

### Plans

| Function | Returns | Notes |
|----------|---------|-------|
| `nsdb_plan_t *ns_plan_fcreate (nsdb_t *db, const char *fmt, ...)` | plan, `NULL` on failure | compiles a query |
| `void ns_plan_free (nsdb_plan_t *plan)` | | free before closing the database |
| `int ns_plan_execute (nsdb_plan_t *plan, txn_t *tx)` | `0` on success | |
| `sb_size ns_plan_read (nsdb_plan_t *plan, txn_t *tx, void *dest, b_size dlen)` | elements read, `< 0` on failure | |
| `sb_size ns_plan_write (nsdb_plan_t *plan, txn_t *tx, const void *src, b_size slen)` | elements written, `< 0` on failure | |
| `void *ns_plan_malloc (nsdb_plan_t *plan, txn_t *tx, b_size *dlen)` | buffer, `NULL` on failure | release with `free` |
| `nsdb_var_t *ns_plan_get_var (nsdb_plan_t *plan, txn_t *tx)` | handle, `NULL` on failure | free with `ns_var_free` |

### Errors

| Function | Returns | Notes |
|----------|---------|-------|
| `const char *ns_strerror (nsdb_t *ns)` | message | describes the last failure on this handle |
| `const char *ns_plan_strerror (nsdb_plan_t *plan)` | message | describes the last failure on this plan |

### Smart files

| Function | Returns | Notes |
|----------|---------|-------|
| `sb_size ns_smfile_size (nsdb_t *smf, txn_t *tx)` | bytes | size under `tx`'s view |
| `sb_size ns_smfile_insert (nsdb_t *smf, txn_t *tx, const void *src, sb_size bofst, b_size slen)` | bytes inserted, `< 0` on failure | pushes `slen` bytes in at byte offset `bofst` |
| `sb_size ns_smfile_write (nsdb_t *smf, txn_t *tx, const void *src, t_size size, sb_size bofst, sb_size stride, b_size nelem)` | elements written, `< 0` on failure | overwrites `nelem` elements of `size` bytes, stepping `stride` |
| `sb_size ns_smfile_read (nsdb_t *smf, txn_t *tx, void *dest, t_size size, sb_size bofst, sb_size stride, b_size nelem)` | elements read, `< 0` on failure | same addressing, reading |
| `sb_size ns_smfile_remove (nsdb_t *smf, txn_t *tx, void *dest, t_size size, sb_size bofst, sb_size stride, b_size nelem)` | elements removed, `< 0` on failure | removes and optionally hands the bytes back in `dest` |

Types and constants
-------------------

| Name | Underlying | What it is |
|------|------------|------------|
| `nsdb_t` | opaque | a database or smart file handle |
| `txn_t` | opaque | a transaction |
| `nsdb_var_t` | opaque | a variable metadata handle |
| `nsdb_plan_t` | opaque | a compiled query |
| `t_size` | `uint32_t` | the size of one type, in bytes |
| `b_size` | `uint64_t` | a byte count or byte offset |
| `sb_size` | `int64_t` | a signed `b_size`; negative means failure |
| `nsdb_var_consumer` | `void (*) (nsdb_var_t *, void *)` | the `ns_visit_nsdb_vars` callback |
| `MAX_ERRSTR_LEN` | `256` | big enough for any Numstore error message |
| `NS_END` | `INT64_MAX` | as a smart file `nelem`, "to the end of the file" - see the warning in [Smart files](#smart-files) |

Print `b_size` with `PRIu64` and `sb_size` with `PRId64` from `<inttypes.h>`,
which `numstore.h` already includes.
