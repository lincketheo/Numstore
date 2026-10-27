Notes for C
===========

Read this when the code is C. These are the C spellings of the ideas in the
other references. For C memory and string hazards, see
`security-memory-safety.md` and `security-unsafe-functions.md`.

- Errors go through an out parameter, and the return value is the status.
  Propagate with a trace macro. Functions that can't fail return void.
- Use `goto` to a single cleanup label at the bottom, with every handle
  initialized to its empty state before the first jump.
- Make everything that isn't in the header `static`. Use `const` for
  parameters and locals that don't change.
- Define the invariant check once per struct with a macro, and call it at
  function entry. Compile asserts out of release builds.
- Put tests in a block guarded by the debug define, directly after the
  function.
- Initialize structs with designated initializers, so a new field is
  visible at every site that builds one.
