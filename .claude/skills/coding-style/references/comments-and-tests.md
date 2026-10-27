Comments and tests
==================

Read this when you write comments, or write tests for the code you changed. If
`tiger-style-naming-and-comments.md` disagrees about comments, this file wins.

Comments
--------

Comments should be terse and simple. They don't describe what is going on;
the code does that. If a comment restates the next line, delete it.

- Don't explain why you did it this way instead of that way, unless "that way"
  is the obvious thing a reader would try. The history of a bug is not a
  comment. "Read the docs first because otherwise this breaks" is not helpful:
  the breakage was a bug, and it's fixed. But if the thing that would break is
  a real subsystem, one a reader could easily forget exists, then say so.
- Visual comments are best. Avoid words; prefer diagrams. Draw the layout,
  the state or the before and after, with a marker under the variable:

  ```text
  # [+++++++++++_______]
  #             ^
  #            idx
  # cur -> limit
  # cur -> new -> limit
  ```

- A short comment at a branch that states the state it is in is worth a lot.
  A comment that states a rule the code relies on ("we go left, then right, so
  we never need to go right again") is worth even more.
- Mark known gaps with a TODO that says what is missing and where the
  tracking issue is, if there is one.

Tests live next to the code
---------------------------

If your language and build allow it, put each function's tests directly below
the function. The function, its tests and its failure cases are then read and
changed together, and a function without a test is visible.

- Name the test after the function. Give each scenario a named test case:
  the normal case, each edge, and each failure.
- Test failure paths with fault injection: make the allocation fail, the write
  come up short, the process crash. For a failing constructor, check that the
  caller's data is untouched.
- Tests build their fixtures through small helpers. When a helper builds a
  complicated structure, a comment says what shape it builds and what the
  inputs mean, ideally with a diagram.
- If tests construct states by hand, add a test that shows those states are
  reachable by the real code. Otherwise the tests may be proving things about
  states that never happen.
- If the project has branch markers (a marker call in a hard to reach branch
  that a test can check), use them, so a test proves it reached the case it
  claims to cover.
- Test-only code is compiled only in test and debug builds.

For how to structure the tests themselves, see the `writing-good-tests`
skill.
