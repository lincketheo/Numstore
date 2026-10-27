Assertions
==========

Read this when you add checks, preconditions or invariants, or review code
that has too few of them. For more rules on assertions (pair assertions,
compile-time assertions, positive and negative space), see
`tiger-style-safety.md`.

Use assertions. They are important. They validate the state of the program,
and writing them is how you define the program as you write it.

- Assert preconditions at the top of a function: sizes are positive, pointers
  are non-null, offsets are in range.
- Assert the state you expect after a transition, before you rely on it.
  After a branch or a loop that is supposed to leave things in a known state,
  say what that state is:

  ```text
  if page_is_held(limit):
      ...
  # the branch above must leave exactly these three states
  assert prev.mode  == NONE
  assert cur.mode   == EXCLUSIVE
  assert next.mode  == NONE
  ```

- Assert the state of structures, not only of arguments. Write an invariant
  check for each data structure, then call it at the entry of every function
  that takes the structure and at the end of every function that builds or
  changes it:

  ```text
  invariant(buffer b):
      assert b != null
      assert b.data != null
      assert b.count <= b.capacity

  append(b, ...):
      invariant(b)
      ...
  ```

- A wall of blanket asserts at the top of a function is okay, but it may mean
  you should assert the state of something instead. If the same four asserts
  keep showing up for the same structure, that is an invariant function that
  hasn't been written yet.
- Mark impossible code as impossible: an `unreachable` marker in the arm of a
  switch that can't happen. Switch exhaustively over enums with no default
  case, so the compiler flags a new enum value that nobody handled.
- Assertions are for bugs in the program, not for failures in the world. Bad
  input, a full disk or a failed allocation is an error to return, never an
  assertion.
