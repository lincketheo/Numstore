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
#include "core/ns_stdtypes.h"
#include "core/ns_testing.h"
#include "core/os/ns_malloc.h"
#include "nscore/pager/ns_pager.h"

#ifndef NDEBUG

/**
TEST_DISABLED (aries_crash)
{
  TEST_CASE ("sample5_bug")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");

    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "AAAAAAAAAA", 0, 10);
    smfile_commit (smf, tx);
    smfile_close (smf);

    smf = smfile_open ("testdb");
    tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "BB", 3, 2);
    smfile_commit (smf, tx);
    smfile_close (smf);

    smf = smfile_open ("testdb");
    tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "CC", 7, 2);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "AAABBAACCAAAAA";
    char        actual[sizeof ("AAABBAACCAAAAA") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

  // 1. Uncommitted work after a crash must be discarded.
  TEST_CASE ("crash_before_commit_discards_uncommitted")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");
    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "AAAAAAAAAA", 0, 10);
    smfile_commit (smf, tx);
    smfile_close (smf);

    smf = smfile_open ("testdb");
    tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "ZZ", 3, 2);
    // deliberately no commit
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "AAAAAAAAAA";
    char        actual[sizeof ("AAAAAAAAAA") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //2. Committed txn followed by uncommitted txn in the same session.
   //   Recovery must keep the first, throw away the second.
  TEST_CASE ("crash_keeps_committed_drops_followon_uncommitted")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");

    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "HELLO", 0, 5);
    smfile_commit (smf, tx);

    tx = smfile_begin (smf);
    smfile_insert (smf, tx, "XX", 1, 2);
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "HELLO";
    char        actual[sizeof ("HELLO") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //3. Multiple crash/reopen cycles. Each session commits, then crashes;
   //   every committed write must survive across all of them.
  TEST_CASE ("repeated_crash_recover_cycles_preserve_all_commits")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);

    smfile_t      *smf = smfile_open ("testdb");
    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "ONE", 0, 3);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf = smfile_open ("testdb");
    tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "TWO", 3, 3);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf = smfile_open ("testdb");
    tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "THREE", 6, 5);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "ONETWOTHREE";
    char        actual[sizeof ("ONETWOTHREE") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //4. Crash with no commits at all in the new session.
   //   File must look exactly as it did at the previous clean close.
  TEST_CASE ("crash_with_no_new_commits_is_a_noop")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");
    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "STABLE", 0, 6);
    smfile_commit (smf, tx);
    smfile_close (smf);

    smf = smfile_open ("testdb");
    ///open, do nothing, crash
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "STABLE";
    char        actual[sizeof ("STABLE") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //5. Many tiny committed transactions, then crash.
   //   Stresses WAL replay across a long redo chain.
  TEST_CASE ("many_small_commits_then_crash")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t *smf = smfile_open ("testdb");

    for (int i = 0; i < 26; i++) {
      char           c  = (char)('A' + i);
      struct txn *tx = smfile_begin (smf);
      smfile_insert (smf, tx, &c, i, 1);
      smfile_commit (smf, tx);
    }
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char        actual[sizeof ("ABCDEFGHIJKLMNOPQRSTUVWXYZ") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //6. Several inserts inside a single transaction, then commit, then crash.
   //   Either ALL of them survive or NONE of them - atomicity check.

  TEST_CASE ("multi_insert_single_txn_is_atomic_through_crash")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");

    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "AAAA", 0, 4);
    smfile_insert (smf, tx, "BB", 2, 2);
    smfile_insert (smf, tx, "CC", 0, 2);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    ///AAAA -> AABBAA -> CCAABBAA
    smf                  = smfile_open ("testdb");
    const char *expected = "CCAABBAA";
    char        actual[sizeof ("CCAABBAA") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //7. Append at the exact end-of-file boundary, then crash.
   //   Catches off-by-ones where offset == length is treated as out-of-range.

  TEST_CASE ("append_at_end_offset_then_crash")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");
    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "ABCDE", 0, 5);
    smfile_commit (smf, tx);
    smfile_close (smf);

    smf = smfile_open ("testdb");
    tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "FGH", 5, 3); ///offset == current length
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "ABCDEFGH";
    char        actual[sizeof ("ABCDEFGH") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //8. Insert at offset 0 into a non-empty file shifts everything right.
   //   Recovery must preserve the shift.

  TEST_CASE ("insert_at_zero_shifts_existing_through_crash")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");
    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "WORLD", 0, 5);
    smfile_commit (smf, tx);
    smfile_close (smf);

    smf = smfile_open ("testdb");
    tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "HELLO ", 0, 6);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf                  = smfile_open ("testdb");
    const char *expected = "HELLO WORLD";
    char        actual[sizeof ("HELLO WORLD") - 1];
    smfile_read (smf, NULL, actual, 1, 0, 1, sizeof (actual));
    test_assert_memequal (expected, actual, sizeof (actual));
    smfile_close (smf);
  }

   //9. Single insert larger than one page - exercises multi-page redo.
   //   Adjust BIG_SIZE to comfortably exceed your page size.

  TEST_CASE ("large_page_spanning_insert_then_crash")
  {
    enum
    {
      BIG_SIZE = 16384
    };

    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);

    char *big = mem.table->malloc (mem.self, BIG_SIZE, 1, &e);
    for (int i = 0; i < BIG_SIZE; i++) {
      big[i] = (char)('A' + (i % 26));
    }

    smfile_t      *smf = smfile_open ("testdb");
    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, big, 0, BIG_SIZE);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf          = smfile_open ("testdb");
    char *actual = mem.table->malloc (mem.self, BIG_SIZE, 1, &e);
    smfile_read (smf, NULL, actual, 1, 0, 1, BIG_SIZE);
    test_assert_memequal (big, actual, BIG_SIZE);
    smfile_close (smf);
    mem.table->free (mem.self, big);
    mem.table->free (mem.self, actual);
  }

   //10. Reads at the boundary of recovered content.
   //    Read from the tail end of the file after recovery, where partial
   //    reads / overruns are most likely to misbehave.

  TEST_CASE ("tail_read_after_recovery")
  {
    error e = error_create ();
    pgr_delete_single_file ("testdb", os, &e);
    smfile_t      *smf = smfile_open ("testdb");
    struct txn *tx  = smfile_begin (smf);
    smfile_insert (smf, tx, "0123456789", 0, 10);
    smfile_commit (smf, tx);
    smfile_crash (smf);

    smf = smfile_open ("testdb");
    ///read the last 4 bytes
    char actual[4];
    smfile_read (smf, NULL, actual, 1, 6, 1, sizeof (actual));
    test_assert_memequal ("6789", actual, sizeof (actual));
    smfile_close (smf);
  }
}
*/

#endif
