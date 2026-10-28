Simtest bug reproductions
=========================

One script per failing `ns_simtest` run. Each holds the tail of the run
commented out, a `FAILS ON` header saying which step broke and what the
sequence into it was, and the command that reproduces it.

See the `debugging-corruption` skill for the page layout, the invariants, and
which `apps/` tool answers which question. Working one of these down to a
regression goes in `csrc/nscore/testing/regressions/`.


Seeds only hold against a build with the same RNG draw sequence
---------------------------------------------------------------

A seed fixes the random *stream*, not the action sequence. Anything that
changes how many draws the run makes shifts everything after the first extra
draw, and the seed then replays a different run.

This bit twice, and the second time was not obvious: fault injection shares
the RNG with action selection, and `ns_faulty_os.c` used to call `randf()`
*before* comparing against `fail_percent`, so **every fallible OS call burned
a draw even at 0%**. Adding a read-only validation pass was therefore enough
to invalidate the whole corpus. Both sites now skip the draw when the rate is
0, which was verified by adding a redundant `ns_db_validate_all` call and
confirming a seed still failed at the identical step.

So at `--fail-percent 0` the stream now survives changes in I/O volume. It
still will not survive changes to how the *operation generator* draws (an
earlier fix to `var_random_name` added a retry loop and invalidated 31
scripts). When you touch the generator, re-hunt and re-file.


The bugs, as of this corpus
---------------------------

**Every one of the five fails on a reopen** — two on `crash_and_reopen`,
three on `close_and_reopen`. Not one fails on an ordinary data operation.

**Four of the five have a `delete` inside a transaction within a few steps of
the failure.** Deletes free pages, which makes "a freed page handed back out,
or not restored on undo" the single most likely mechanism behind the group.

| #    | Step | Fails on   | Symptom                              |
| ---- | ---- | ---------- | ------------------------------------ |
| 0001 | 1204 | crash      | rope root has no elements            |
| 0002 | 1526 | close      | rope layer page type is garbage      |
| 0003 | 702  | crash      | rope points at a `PG_VAR_PAGE`       |
| 0004 | 1237 | close      | directory is one variable short      |
| 0005 | 1352 | close      | bottom layer vs root size off by 4.5x |

They split into two flavours, and the split matters because it points at
different code:

- **Structure** (0001, 0002, 0003): a pointer leads to a page that is not
  what it should be — unset, garbage, or belonging to the variable directory.
  0003 getting `PG_VAR_PAGE` from a *rope* pointer is the clearest single
  piece of evidence for page reuse.
- **Bookkeeping** (0004, 0005): every page walks fine, but a count is wrong —
  one missing directory entry, or a root tracking 4.5x the bytes its leaves
  hold. Notably 0005's transaction **committed**, so that one is not an undo
  bug at all.


How to approach them
--------------------

Roughly in order of expected payoff:

1. **Start with 0004, then 0003.** 0004 has no corrupt page to interpret —
   just a missing entry — and 0003 fails at step 702, by far the earliest, so
   it iterates fastest.

2. **Bisect by action class, not by guessing shapes.** `--disable ACTION` is
   repeatable, so replay a seed with `NSS_DELETE_CURRENT_VARIABLE_AND_SWITCH`
   disabled, then `NSS_REMOVE`, and so on, until the failure disappears. That
   names the class the bug needs. Hand-written shapes were tried and all
   pass: delete-in-txn then crash at 1/2/5/10 variables, names long enough to
   need `PG_VAR_TAIL` overflow, and 40 plain crash cycles with create/delete
   churn. Guessing shapes does not work here.

3. **Settle whether recovery causes the damage or only reveals it.** The
   per-step checks read *through the pager*, so "validated clean" means clean
   in the buffer pool, which a reopen discards. For a failing seed, compare
   the on-disk image just before the reopen against the one after it —
   `ns_nsspprint` on both (raw, no validation, so it will show the garbage
   page) — and use `ns_walpprint` to find which records touched that page
   number and whether its `pageLSN` is older than the last record that should
   have applied to it. This decides whether the fix belongs in recovery or in
   what gets flushed, and nothing below it is worth doing first.

4. **For 0005 specifically, skip the pages.** Nothing is structurally wrong,
   so diff `ns_rope_page_stats` across the bottom layer against the root's
   `tracked`, and find which operation in the committed transaction stopped
   maintaining the total.

5. **Use the validators as a bisector.** `ns_rope_validate` and the directory
   check are cheap enough to call more often than they currently are. Moving
   `ns_db_validate_all` from the reopen paths to *every* step would be far too
   slow to leave in, but it is the fastest way to find the exact operation
   that breaks an invariant on one known-bad seed.


What was written while producing this corpus
--------------------------------------------

New regression:

- `csrc/nscore/testing/regressions/0004_crash_after_churn_diverges.c`, plus
  its entry in that directory's `CMakeLists.txt`. It drives the simulation
  stepper directly — a long run of data churn, then exactly one crash,
  repeated — and reproduces the rope corruption at step 545: an inner node
  pointing at a page whose type byte is garbage, the same family as 0002 and
  0003 above.
  **It fails on purpose.** It also trips
  `atomic_load (&os->fds[i]) == -1` (`ns_posix_os.c:44`) on teardown, a
  second defect in the crash path's cleanup which aborts the process, so
  tests ordered after it do not run.

Harness, to catch this class at its source instead of hundreds of steps later:

- `nss_validate_variables` in `ns_numstore_simulation.c` — after every
  operation, compares the database's variable set against the reference's.
  Gated to outside a transaction, since `ns_visit_variables` takes no txn and
  so cannot see uncommitted work.
- `ns_db_validate_all` in `ns_db_state_machine.c` — runs `ns_rope_validate`
  over every variable, not just the current one. Called on both reopen paths
  only; it scans each bottom layer, so per-step would be far too slow.
- `ns_db_visit_vars` and `ns_ref_var_exists_str`, the two accessors the above
  needed.
- `ns_simul_set_enabled` in `ns_numstore_simulation.c` — lets a caller shape
  the action sequence, which is what the new regression uses to put churn and
  a crash in a fixed pattern.

Fixes:

- `ns_faulty_os.c` — skip the fault-injection draw at `fail_percent == 0`, so
  seeds survive changes in I/O volume (see above).
- `lex_is_keyword` in `ns_lexer.c`/`.h`, with the retry loop in
  `var_random_name` (`ns_variables.c`). The generator could emit reserved
  words as variable names, so the simulation issued `create if union {...}`
  and it would not compile. Non-obvious part: a word is scanned as a
  primitive type *before* it is checked against the keywords, so `i8` and
  `u32` are reserved too, not just the 15 keywords. Covered by two unit
  tests — `lex_is_keyword` in `ns_lexer.c`, and the invariant added to
  `var_random_name` in `ns_variables.c`.
