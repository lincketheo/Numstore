---
name: coding-style
description: Code conventions for writing and reviewing code in any language: simple code, function intent and locality, assertions, error handling, terse visual comments, tests beside code.
---

Coding style and conventions
============================

Use this whenever you write, extend, refactor or review code. It is about how
code is structured and what it means, not how it looks. The principles apply
to any language; the examples are pseudo code, with C notes at the end because
the reference code was C.

Not covered here, on purpose: formatting, whitespace, file headers and banner
comments. A formatter and the surrounding code own those. Match what is
already in the file.

The core ideas, in priority order:

1. Functions capture intent. This is the most important rule.
2. Code is simple and straightforward, and reads top to bottom.
3. The state of the program is asserted, not assumed.
4. Failure is handled on purpose, and leaves things as they were.
5. Comments are terse and visual. Tests sit next to the code they test.

Simplicity
----------

- Write the most straightforward code that works. Prefer plain control flow
  and plain data over cleverness, indirection and generality.
- Don't add an abstraction for a single use. Don't add a parameter, option or
  layer for a case that doesn't exist yet.
- Spelled-out, parallel code is fine when each copy is a distinct thing, such
  as one writer and one reader per record format. Don't merge them behind a
  generic mechanism just to write fewer lines. A little repetition is cheaper
  than the wrong abstraction.
- Flat beats nested: return or jump out early on failure so the happy path
  stays at one indent level.
- The fewer concepts a reader has to hold, the better. Before adding a new
  helper, type or name, ask whether the code is simpler to understand with it
  or without it.

Functions: length and locality
------------------------------

Functions should be short, but locality matters more than shortness. Reading a
function top to bottom should tell you what it does without chasing calls
around the file.

- A long, straightforward function is better than several small, meaningless
  ones. Don't split a function only because it is long.
- A function that walks through a state machine or a multi-step algorithm
  often should stay in one piece, with comments marking each state, so the
  whole algorithm can be read in one place.
- Every function you extract costs the reader a jump. It has to earn that jump
  by giving them a name for something real.

Splitting functions
-------------------

Splitting functions into sub functions must be done carefully, but often.
Carefully, because every split must pay for itself. Often, because code that
mixes several real concepts in one body is hard to read and hard to test.

Split by the intent of the system, not by the intent of the local variables
you happen to be working on. Think in systems, not in shorthand. The question
is never "what would let me write less code?" It is "what is a thing in this
system that deserves a name?"

### A function must not lie

A function does what its name says: all of it, and nothing else. A long name
is a symptom of a bad local function. If the honest name lists the steps, the
function isn't a concept.

Suppose you pulled this out:

```text
step:
    bar += 1
    biz += 1
    wal_write(biz, bar)
    foo.biz.run()
```

This is a good function only if a step is defined by the higher level system
as:

1. Increment bar
2. Increment biz
3. Write to the wal
4. Run foo's biz

If that is not what a step means in the system, and you extracted it only to
shorten the caller, then the honest name is:

```text
increment_bar_and_biz_write_and_run_biz:
    ...
```

That name is absurd, and that is the signal: it was a bad local function.
Inline it back, or find the real concept hiding in it.

### Define concepts, but few

Functions capture intent. You can define a "step" as something that performs
those actions, but too many definitions are bad: every definition is something
the reader has to learn. Name the things that are real, and leave the rest
inline.

A good example of a good definition is commit:

```text
commit:
    append a commit log
    flush all
    append an end log
    remove txn
```

If you didn't know what a commit means, this would be a horrible local
function. Because commit is a defined property of the system, with a clear
meaning, it's right. The body is the definition.

### Checklist before extracting

- Is this a concept the system already has, or one that I'm inventing to
  save lines?
- Can I name it with a short, honest name that matches everything it does?
- Does the caller read better, or does the reader now have to open this
  function to understand the caller?
- If I deleted the function and inlined the body, would the code get harder to
  understand? If not, don't extract it.

Assertions
----------

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

Errors and cleanup
------------------

- Use one error mechanism for the whole project, and propagate with one idiom.
  Don't invent a second way to fail in a corner of the code.
- Failures are specific and carry context: what went wrong and the values
  involved (the page number, the expected and actual lengths), not just
  "failed". Keep distinct causes distinct. A corrupt file is not an
  out-of-memory error is not a bad argument.
- Check every call that can fail. Propagate it, or handle it on purpose and
  say so.
- Failure leaves the world as it was. Do all the fallible work first, and
  write to the caller's outputs only once nothing can fail any more. Then a
  failed call doesn't leave a half-written structure behind:

  ```text
  create(dest, size, cap, err):
      assert size > 0
      assert cap > 0

      data = allocate(cap, size, err)
      if data is null:
          return trace(err)

      # nothing above touched dest
      dest = Buffer{ size, count: 0, capacity: cap, data }
      invariant(dest)
      return ok
  ```

- When a function acquires several resources, give every one a known empty
  state first, then release whichever were acquired through one cleanup path.
  Cleanup operations that are safe on an empty or already released resource
  make that path trivial to get right:

  ```text
  work(...):
      prev = empty_handle()
      next = empty_handle()

      if acquire(prev) fails: goto failed
      if acquire(next) fails: goto failed
      ...
      return ok

  failed:
      cancel_if_held(prev)    # safe when empty
      cancel_if_held(next)
      return trace(err)
  ```

  In a language with destructors, `defer` or `finally`, use that instead of a
  jump. The idea is the same: one place releases everything.

Ownership and resources
-----------------------

- Every resource has exactly one owner at a time. When ownership moves, move
  it explicitly and leave the source empty, so a stale copy can't be released
  twice or used after the move.
- Group lifetimes where you can: allocate related objects from one arena or
  region and free them together, instead of tracking each pointer.
- Make mutability visible at the use site. Prefer separate read-only and
  writable accessors over one accessor that can do both, so every write is easy
  to find with a search.
- Use fixed-width types for anything persisted or sent over the wire. Encode
  and decode fields explicitly; don't dump a struct's memory layout into a
  file. Checksum records, verify the checksum on read, and report a mismatch
  as corruption.

Interfaces and testability
--------------------------

- Pass the environment in: the allocator, filesystem, clock, threads. Don't
  reach for global ones. Then a test can substitute an allocator that fails
  after N calls, a filesystem that returns short reads, or a thread
  primitive that records what happened.
- Design failure in. If the system must survive a crash, give it a way to
  simulate one (a crash entry point that drops state without flushing), so
  recovery is testable without killing a process.
- Keep the public surface small: everything not needed outside the module
  is private. In a header, expose the API and forward declare types instead of
  including their definitions.

Naming
------

Names carry contracts, not just topics.

- Prefix public functions with a short module name (`wal_`, `pgr_`), so the
  owner of a function is obvious at the call site.
- A suffix states a contract the caller must know about: `_locked` (the
  caller already holds the lock), `_if_exists` (safe when there is nothing to
  do), `_or_null` (accepts or returns null), `_internal`.
- Predicates read as questions (`done_left`, `is_new`). Constructors and
  destructors come in pairs (`open`/`close`, `create`/`destroy`).
- Name by what the thing is in the system, not by how it is currently
  implemented.

Concurrency
-----------

- Take locks at the boundary of the public function and release them at one
  exit. Helpers that assume the lock is held say so in their name
  (`_locked`) and never take it themselves.
- Hold a lock for the shortest region that keeps the invariant. Check the
  structure's invariants inside the lock, where they are guaranteed to hold.
- Prefer atomics for counters and flags read without the lock, and document
  which fields each lock protects.
- Anything with locks or concurrency primitives should have a randomized
  simulation test. See the `writing-good-tests` skill.

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

Think Before Coding
-------------------

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

Simplicity First
----------------

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

Surgical Changes
----------------

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

Goal-Driven Execution
---------------------

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.


Notes for C
-----------

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

Safety 
------
- - **Put a limit on everything** because, in reality, this is what we expect—everything has a limit.
  For example, all loops and all queues must have a fixed upper bound to prevent infinite loops or
  tail latency spikes. This follows the [“fail-fast”](https://en.wikipedia.org/wiki/Fail-fast)
  principle so that violations are detected sooner rather than later. Where a loop cannot terminate
  (e.g. an event loop), this must be asserted.
- Use explicitly-sized types like `u32` for everything, avoid architecture-specific `usize`.
- **Assertions detect programmer errors. Unlike operating errors, which are expected and which must
  be handled, assertion failures are unexpected. The only correct way to handle corrupt code is to
  crash. Assertions downgrade catastrophic correctness bugs into liveness bugs. Assertions are a
  force multiplier for discovering bugs by fuzzing.**

  - **Assert all function arguments and return values, pre/postconditions and invariants.** A
    function must not operate blindly on data it has not checked. The purpose of a function is to
    increase the probability that a program is correct. Assertions within a function are part of how
    functions serve this purpose. The assertion density of the code must average a minimum of two
    assertions per function.

  - **[Pair assertions](https://tigerbeetle.com/blog/2023-12-27-it-takes-two-to-contract).** For
    every property you want to enforce, try to find at least two different code paths where an
    assertion can be added. For example, assert validity of data right before writing it to disk,
    and also immediately after reading from disk.

  - On occasion, you may use a blatantly true assertion instead of a comment as stronger
    documentation where the assertion condition is critical and surprising.

  - Split compound assertions: prefer `assert(a); assert(b);` over `assert(a and b);`.
    The former is simpler to read, and provides more precise information if the condition fails.

  - Use single-line `if` to assert an implication: `if (a) assert(b)`.

  - **Assert the relationships of compile-time constants** as a sanity check, and also to document
    and enforce [subtle
    invariants](https://github.com/coilhq/tigerbeetle/blob/db789acfb93584e5cb9f331f9d6092ef90b53ea6/src/vsr/journal.zig#L45-L47)
    or [type
    sizes](https://github.com/coilhq/tigerbeetle/blob/578ac603326e1d3d33532701cb9285d5d2532fe7/src/ewah.zig#L41-L53).
    Compile-time assertions are extremely powerful because they are able to check a program's design
    integrity _before_ the program even executes.

  - **The golden rule of assertions is to assert the _positive space_ that you do expect AND to
    assert the _negative space_ that you do not expect** because where data moves across the
    valid/invalid boundary between these spaces is where interesting bugs are often found. This is
    also why **tests must test exhaustively**, not only with valid data but also with invalid data,
    and as valid data becomes invalid.

  - Assertions are a safety net, not a substitute for human understanding. With simulation testing,
    there is the temptation to trust the fuzzer. But a fuzzer can prove only the presence of bugs,
    not their absence. Therefore:
    - Build a precise mental model of the code first,
    - encode your understanding in the form of assertions,
    - write the code and comments to explain and justify the mental model to your reviewer,
    - and use VOPR as the final line of defense, to find bugs in your and reviewer's understanding
      of code.
- Declare variables at the **smallest possible scope**, and **minimize the number of variables in
  scope**, to reduce the probability that variables are misused.
  * Good function shape is often the inverse of an hourglass: a few parameters, a simple return
    type, and a lot of meaty logic between the braces.
  * Centralize control flow. When splitting a large function, try to keep all switch/if
    statements in the "parent" function, and move non-branchy logic fragments to helper
    functions. Divide responsibility. All control flow should be handled by _one_ function, the rest shouldn't
    care about control flow at all. In other words,
    ["push `if`s up and `for`s down"](https://matklad.github.io/2023/11/15/push-ifs-up-and-fors-down.html).
  * Similarly, centralize state manipulation. Let the parent function keep all relevant state in
    local variables, and use helpers to compute what needs to change, rather than applying the
    change directly. Keep leaf functions pure.
    - Compound conditions that evaluate multiple booleans make it difficult for the reader to verify
  that all cases are handled. Split compound conditions into simple conditions using nested
  `if/else` branches. Split complex `else if` chains into `else { if { } }` trees. This makes the
  branches and cases clear. Again, consider whether a single `if` does not also need a matching
  `else` branch, to ensure that the positive and negative spaces are handled or asserted.

- Negations are not easy! State invariants positively. When working with lengths and indexes, this
  form is easy to get right (and understand):

  ```zig
  if (index < count) {
    // The invariant holds.
  } else {
    // The invariant doesn't hold.
  }
  ```

  This form is harder, and also goes against the grain of how `index` would typically be compared to
  `count`, for example, in a loop condition:

  ```zig
  if (index >= count) {
    // It's not true that the invariant holds.
  }
  ```
- **Explicitly pass options to library functions at the call site, instead of relying on the
  defaults**. For example, write `@prefetch(a, .{ .cache = .data, .rw = .read, .locality = 3 });`
  over `@prefetch(a, .{});`. This improves readability but most of all avoids latent, potentially
  catastrophic bugs in case the library ever changes its defaults.

  > “The lack of back-of-the-envelope performance sketches is the root of all evil.” — Rivacindela
> Hudsoni

- Think about performance from the outset, from the beginning. **The best time to solve performance,
  to get the huge 1000x wins, is in the design phase, which is precisely when we can't measure or
  profile.** It's also typically harder to fix a system after implementation and profiling, and the
  gains are less. So you have to have mechanical sympathy. Like a carpenter, work with the grain.

- **Perform back-of-the-envelope sketches with respect to the four resources (network, disk, memory,
  CPU) and their two main characteristics (bandwidth, latency).** Sketches are cheap. Use sketches
  to be “roughly right” and land within 90% of the global maximum.

- Optimize for the slowest resources first (network, disk, memory, CPU) in that order, after
  compensating for the frequency of usage, because faster resources may be used many times more. For
  example, a memory cache miss may be as expensive as a disk fsync, if it happens many times more.

- Distinguish between the control plane and data plane. A clear delineation between control plane
  and data plane through the use of batching enables a high level of assertion safety without losing
  performance. See our [July 2021 talk on Zig SHOWTIME](https://youtu.be/BH2jvJ74npM?t=1958) for
  examples.

- Amortize network, disk, memory and CPU costs by batching accesses.

- Let the CPU be a sprinter doing the 100m. Be predictable. Don't force the CPU to zig zag and
  change lanes. Give the CPU large enough chunks of work. This comes back to batching.

- Be explicit. Minimize dependence on the compiler to do the right thing for you.

  In particular, extract hot loops into stand-alone functions with primitive arguments without
  `self` (see [an example](https://github.com/tigerbeetle/tigerbeetle/blob/0.16.19/src/lsm/compaction.zig#L1932-L1937)).
  That way, the compiler doesn't need to prove that it can cache struct's fields in registers, and a
  human reader can spot redundant computations easier.

  - **Get the nouns and verbs just right.** Great names are the essence of great code, they capture
  what a thing is or does, and provide a crisp, intuitive mental model. They show that you
  understand the domain. Take time to find the perfect name, to find nouns and verbs that work
  together, so that the whole is greater than the sum of its parts.

- Use `snake_case` for function, variable, and file names. The underscore is the closest thing we
  have as programmers to a space, and helps to separate words and encourage descriptive names. We
  don't use Zig's `CamelCase.zig` style for "struct" files to keep the convention simple and
  consistent.

- Do not abbreviate variable names, unless the variable is a primitive integer type used as an
  argument to a sort function or matrix calculation. Use long form arguments in scripts: `--force`,
  not `-f`. Single letter flags are for interactive usage.

- Use proper capitalization for acronyms (`VSRState`, not `VsrState`).

- For the rest, follow the Zig style guide.

- Add units or qualifiers to variable names, and put the units or qualifiers last, sorted by
  descending significance, so that the variable starts with the most significant word, and ends with
  the least significant word. For example, `latency_ms_max` rather than `max_latency_ms`. This will
  then line up nicely when `latency_ms_min` is added, as well as group all variables that relate to
  latency.

- Infuse names with meaning. For example, `allocator: Allocator` is a good, if boring name,
  but `gpa: Allocator` and `arena: Allocator` are excellent. They inform the reader whether
  `deinit` should be called explicitly.

- When choosing related names, try hard to find names with the same number of characters so that
  related variables all line up in the source. For example, as arguments to a memcpy function,
  `source` and `target` are better than `src` and `dest` because they have the second-order effect
  that any related variables such as `source_offset` and `target_offset` will all line up in
  calculations and slices. This makes the code symmetrical, with clean blocks that are easier for
  the eye to parse and for the reader to check.

- When a single function calls out to a helper function or callback, prefix the name of the helper
  function with the name of the calling function to show the call history. For example,
  `read_sector()` and `read_sector_callback()`.

- Callbacks go last in the list of parameters. This mirrors control flow: callbacks are also
  _invoked_ last.

- _Order_ matters for readability (even if it doesn't affect semantics). On the first read, a file
  is read top-down, so put important things near the top. The `main` function goes first.

  The same goes for `structs`, the order is fields then types then methods:

  ```zig
  time: Time,
  process_id: ProcessID,

  const ProcessID = struct { cluster: u128, replica: u8 };
  const Tracer = @This(); // This alias concludes the types section.

  pub fn init(gpa: std.mem.Allocator, time: Time) !Tracer {
      ...
  }
  ```

  If a nested type is complex, make it a top-level struct.

  At the same time, not everything has a single right order. When in doubt, consider sorting
  alphabetically, taking advantage of big-endian naming.

- Don't overload names with multiple meanings that are context-dependent. For example, TigerBeetle
  has a feature called _pending transfers_ where a pending transfer can be subsequently _posted_ or
  _voided_. At first, we called them _two-phase commit transfers_, but this overloaded the
  _two-phase commit_ terminology that was used in our consensus protocol, causing confusion.

- Think of how names will be used outside the code, in documentation or communication. For example,
  a noun is often a better descriptor than an adjective or present participle, because a noun can be
  directly used in correspondence without having to be rephrased. Compare `replica.pipeline` vs
  `replica.preparing`. The former can be used directly as a section header in a document or
  conversation, whereas the latter must be clarified. Noun names compose more clearly for derived
  identifiers, e.g. `config.pipeline_max`.

- Zig has named arguments through the `options: struct` pattern. Use it when arguments can be
  mixed up. A function taking two `u64` must use an options struct. If an argument can be `null`,
  it should be named so that the meaning of `null` literal at the call site is clear.

  Because dependencies like an allocator or a tracer are singletons with unique types, they should
  be threaded through constructors positionally, from the most general to the most specific.

- **Write descriptive commit messages** that inform and delight the reader, because your commit
  messages are being read. Note that a pull request description is not stored in the git repository
  and is invisible in `git blame`, and therefore is not a replacement for a commit message.

- Don't forget to say why. Code alone is not documentation. Use comments to explain why you wrote
  the code the way you did. Show your workings.

- Don't forget to say how. For example, when writing a test, think of writing a description at the
  top to explain the goal and methodology of the test, to help your reader get up to speed, or to
  skip over sections, without forcing them to dive in.

- Comments are sentences, with a space after the slash, with a capital letter and a full stop, or a
  colon if they relate to something that follows. Comments are well-written prose describing the
  code, not just scribblings in the margin. Comments after the end of a line _can_ be phrases, with
  no punctuation.

  - Don't duplicate variables or take aliases to them. This will reduce the probability that state
  gets out of sync.

- If you don't mean a function argument to be copied when passed by value, and if the argument type
  is more than 16 bytes, then pass the argument as `*const`. This will catch bugs where the caller
  makes an accidental copy on the stack before calling the function.

- Construct larger structs _in-place_ by passing an _out pointer_ during initialization.

  In-place initializations can assume **pointer stability** and **immovable types** while
  eliminating intermediate copy-move allocations, which can lead to undesirable stack growth.

  Keep in mind that in-place initializations are viral — if any field is initialized
  in-place, the entire container struct should be initialized in-place as well.

  **Prefer:**
  ```zig
  fn init(target: *LargeStruct) !void {
    target.* = .{
      // in-place initialization.
    };
  }

  fn main() !void {
    var target: LargeStruct = undefined;
    try target.init();
  }
  ```

  **Over:**
  ```zig
  fn init() !LargeStruct {
    return LargeStruct {
      // moving the initialized object.
    }
  }

  fn main() !void {
    var target = try LargeStruct.init();
  }
  ```

- **Shrink the scope** to minimize the number of variables at play and reduce the probability that
  the wrong variable is used.

- Calculate or check variables close to where/when they are used. **Don't introduce variables before
  they are needed.** Don't leave them around where they are not. This will reduce the probability of
  a POCPOU (place-of-check to place-of-use), a distant cousin to the infamous
  [TOCTOU](https://en.wikipedia.org/wiki/Time-of-check_to_time-of-use). Most bugs come down to a
  semantic gap, caused by a gap in time or space, because it's harder to check code that's not
  contained along those dimensions.

- Use simpler function signatures and return types to reduce dimensionality at the call site, the
  number of branches that need to be handled at the call site, because this dimensionality can also
  be viral, propagating through the call chain. For example, as a return type, `void` trumps `bool`,
  `bool` trumps `u64`, `u64` trumps `?u64`, and `?u64` trumps `!u64`.

- Ensure that functions run to completion without suspending, so that precondition assertions are
  true throughout the lifetime of the function. These assertions are useful documentation without a
  suspend, but may be misleading otherwise.

- Be on your guard for **[buffer bleeds](https://en.wikipedia.org/wiki/Heartbleed)**. This is a
  buffer underflow, the opposite of a buffer overflow, where a buffer is not fully utilized, with
  padding not zeroed correctly. This may not only leak sensitive information, but may cause
  deterministic guarantees as required by TigerBeetle to be violated.

- Use newlines to **group resource allocation and deallocation**, i.e. before the resource
  allocation and after the corresponding `defer` statement, to make leaks easier to spo

---
title: Ensure Memory Safety
impact: CRITICAL
impactDescription: Arbitrary code execution and data corruption
tags: security, memory-safety, buffer-overflow, c, cpp, cwe-415, cwe-416, cwe-119
---

## Ensure Memory Safety

Memory safety vulnerabilities are among the most critical security issues in software development. They can lead to arbitrary code execution, data corruption, denial of service, and information disclosure. This guide covers common memory safety issues in C/C++ including double-free, use-after-free, and buffer overflow vulnerabilities.

### Double Free (CWE-415)

Freeing memory twice can cause memory corruption, crashes, or allow attackers to execute arbitrary code.

**Incorrect:**

```c
int bad_code() {
    char *var = malloc(sizeof(char) * 10);
    free(var);
    free(var);  // Double free vulnerability
    return 0;
}
```

**Correct:**

```c
int safe_code() {
    char *var = malloc(sizeof(char) * 10);
    free(var);
    var = NULL;  // Set to NULL after free
    free(var);   // Safe: freeing NULL is a no-op
    return 0;
}
```

### Use After Free (CWE-416)

Accessing memory after it has been freed can lead to crashes, data corruption, or code execution.

**Incorrect:**

```c
typedef struct name {
    char *myname;
    void (*func)(char *str);
} NAME;

int bad_code() {
    NAME *var;
    var = (NAME *)malloc(sizeof(struct name));
    free(var);
    var->func("use after free");  // Accessing freed memory
    return 0;
}
```

**Correct:**

```c
typedef struct name {
    char *myname;
    void (*func)(char *str);
} NAME;

int safe_code() {
    NAME *var;
    var = (NAME *)malloc(sizeof(struct name));
    free(var);
    var = NULL;  // Prevents accidental reuse
    // Any access to var now causes immediate crash (easier to debug)
    return 0;
}
```

### Buffer Overflow (CWE-119, CWE-120)

Writing beyond buffer boundaries can overwrite adjacent memory, leading to crashes or code execution.

**Incorrect:**

```c
void bad_code(char *user_input) {
    char buffer[64];
    strcpy(buffer, user_input);  // No bounds checking
}
```

**Correct:**

```c
void safe_code(char *user_input) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s", user_input);  // Bounds-checked, always null-terminates
}
```

### Format String Vulnerabilities (CWE-134)

Using user-controlled format strings can allow attackers to read or write arbitrary memory.

**Incorrect:**

```c
void bad_printf(char *user_input) {
    printf(user_input);  // User controls format string
}
```

**Correct:**

```c
void safe_printf(char *user_input) {
    printf("%s", user_input);  // Format string is fixed
}
```

### Prevention Best Practices

1. **Set pointers to NULL after freeing** - Prevents use-after-free and double-free
2. **Use bounded string functions** - `snprintf` instead of `strcpy`/`sprintf` (`strncpy` requires manual null-termination — prefer `snprintf`)
3. **Never use user input as format strings** - Always use fixed format strings
4. **Validate array indices** - Check bounds before accessing arrays
5. **Use static analysis tools** - Semgrep, Coverity, or similar to detect issues
6. **Consider memory-safe languages** - Rust, Go, or managed languages where appropriate

---
title: Avoid Unsafe Functions
impact: HIGH
impactDescription: Buffer overflows and memory corruption
tags: security, unsafe-functions, c, php, python, go, rust, cwe-120, cwe-676
---

## Avoid Unsafe Functions

Certain functions in various programming languages are inherently dangerous because they do not perform boundary checks, can lead to buffer overflows, have been deprecated, or bypass type safety mechanisms. Using these functions can result in security vulnerabilities, memory corruption, and arbitrary code execution.

**Incorrect (C - strcat buffer overflow):**

```c
int bad_strcpy(src, dst) {
    n = DST_BUFFER_SIZE;
    if ((dst != NULL) && (src != NULL) && (strlen(dst)+strlen(src)+1 <= n))
    {
        // ruleid: insecure-use-strcat-fn
        strcat(dst, src);

        // ruleid: insecure-use-strcat-fn
        strncat(dst, src, 100);
    }
}
```

**Correct (C - use strcat_s with bounds checking):**

```c
// Use strcat_s which performs bounds checking
```

**Incorrect (C - strcpy buffer overflow):**

```c
int bad_strcpy(src, dst) {
    n = DST_BUFFER_SIZE;
    if ((dst != NULL) && (src != NULL) && (strlen(dst)+strlen(src)+1 <= n))
    {
        // ruleid: insecure-use-string-copy-fn
        strcpy(dst, src);

        // ruleid: insecure-use-string-copy-fn
        strncpy(dst, src, 100);
    }
}
```

**Correct (C - use strcpy_s with bounds checking):**

```c
// Use strcpy_s which performs bounds checking
```

**Incorrect (C - strtok modifies buffer):**

```c
int bad_code() {
    char str[DST_BUFFER_SIZE];
    fgets(str, DST_BUFFER_SIZE, stdin);
    // ruleid:insecure-use-strtok-fn
    strtok(str, " ");
    printf("%s", str);
    return 0;
}
```

**Correct (C - use strtok_r instead):**

```c
int main() {
    char str[DST_BUFFER_SIZE];
    char dest[DST_BUFFER_SIZE];
    fgets(str, DST_BUFFER_SIZE, stdin);
    // ok:insecure-use-strtok-fn
    strtok_r(str, " ", *dest);
    printf("%s", str);
    return 0;
}
```

**Incorrect (C - scanf buffer overflow):**

```c
int bad_code() {
    char str[DST_BUFFER_SIZE];
    // ruleid:insecure-use-scanf-fn
    scanf("%s", str);
    printf("%s", str);
    return 0;
}
```

**Correct (C - use fgets instead):**

```c
int main() {
    char str[DST_BUFFER_SIZE];
    // ok:insecure-use-scanf-fn
    fgets(str);
    printf("%s", str);
    return 0;
}
```

**Incorrect (C - gets buffer overflow):**

```c
int bad_code() {
    char str[DST_BUFFER_SIZE];
    // ruleid:insecure-use-gets-fn
    gets(str);
    printf("%s", str);
    return 0;
}
```

**Correct (C - use fgets or gets_s instead):**

```c
int main() {
    char str[DST_BUFFER_SIZE];
    // ok:insecure-use-gets-fn
    fgets(str);
    printf("%s", str);
    return 0;
}
```

**Incorrect (PHP - deprecated mcrypt functions):**

```php
<?php

// ruleid: mcrypt-use
mcrypt_ecb(MCRYPT_BLOWFISH, $key, base64_decode($input), MCRYPT_DECRYPT);

// ruleid: mcrypt-use
mcrypt_create_iv($iv_size, MCRYPT_RAND);

// ruleid: mcrypt-use
mdecrypt_generic($td, $c_t);
```

**Correct (PHP - use Sodium or OpenSSL):**

```php
<?php

// ok: mcrypt-use
sodium_crypto_secretbox("Hello World!", $nonce, $key);

// ok: mcrypt-use
openssl_encrypt($plaintext, $cipher, $key, $options=0, $iv, $tag);
```

**Incorrect (Python - tempfile.mktemp race condition):**

```python
import tempfile as tf

# ruleid: tempfile-insecure
x = tempfile.mktemp()
# ruleid: tempfile-insecure
x = tempfile.mktemp(dir="/tmp")
```

**Correct (Python - use NamedTemporaryFile):**

```python
import tempfile

# Use NamedTemporaryFile instead
with tempfile.NamedTemporaryFile() as tmp:
    tmp.write(b"data")
```

**Incorrect (Go - unsafe package bypasses type safety):**

```go
package main

import (
	"fmt"
	"unsafe"

	foobarbaz "unsafe"
)

type Fake struct{}

func (Fake) Good() {}
func main() {
	unsafeM := Fake{}
	unsafeM.Good()
	intArray := [...]int{1, 2}
	fmt.Printf("\nintArray: %v\n", intArray)
	intPtr := &intArray[0]
	fmt.Printf("\nintPtr=%p, *intPtr=%d.\n", intPtr, *intPtr)
	// ruleid: use-of-unsafe-block
	addressHolder := uintptr(foobarbaz.Pointer(intPtr)) + unsafe.Sizeof(intArray[0])
	// ruleid: use-of-unsafe-block
	intPtr = (*int)(foobarbaz.Pointer(addressHolder))
	fmt.Printf("\nintPtr=%p, *intPtr=%d.\n\n", intPtr, *intPtr)
}
```

**Correct (Go - avoid unsafe package):**

```go
// Avoid using the unsafe package. Use Go's type-safe alternatives for memory operations.
```

**Incorrect (Rust - calling C FFI without unsafe block):**

```rust
// ruleid: unsafe-usage
// This will not compile — libc::getpid() is an extern "C" function and requires unsafe
let pid = libc::getpid() as u32;
```

**Correct — Option A (Rust - use safe standard library alternative, preferred):**

```rust
// ok: unsafe-usage
// std::process::id() is a safe wrapper that returns the OS-assigned PID
let pid: u32 = std::process::id();
```

**Correct — Option B (Rust - minimally scoped unsafe block with SAFETY comment):**

```rust
// ok: unsafe-usage
// SAFETY: libc::getpid() is a read-only syscall with no preconditions
let pid = unsafe { libc::getpid() } as u32;
```

**Incorrect (OCaml - unsafe functions skip bounds checks):**

```ocaml
let cb = Array.make 10 2 in
(* ruleid:ocamllint-unsafe *)
Printf.printf "%d\n" (Array.unsafe_get cb 12)
```

**Correct (OCaml - use bounds-checked functions):**

```ocaml
let cb = Array.make 10 2 in
(* Use bounds-checked version *)
Printf.printf "%d\n" (Array.get cb 0)
```

---
title: Prevent Path Traversal
impact: CRITICAL
impactDescription: Arbitrary file access, information disclosure, file manipulation
tags: security, path-traversal, cwe-22, cwe-23, cwe-73, cwe-98
---

## Prevent Path Traversal

Path traversal occurs when user input is used to construct file paths without proper validation, allowing attackers to access files outside intended directories using sequences like "../". This can lead to sensitive data exposure, arbitrary file reads/writes, and system compromise.

---

### Language: Python

#### open() Path Traversal

**Incorrect (vulnerable to path traversal):**
```python
def unsafe(request):
    filename = request.POST.get('filename')
    f = open(filename, 'r')
    data = f.read()
    f.close()
    return HttpResponse(data)
```

**Correct (static path):**
```python
def safe(request):
    filename = "/tmp/data.txt"
    f = open(filename)
    data = f.read()
    f.close()
    return HttpResponse(data)
```

**References:**
- CWE-22: Path Traversal
- [OWASP Path Traversal](https://owasp.org/www-community/attacks/Path_Traversal)

---

### Language: JavaScript/Node.js

#### Non-Literal fs Filename

**Incorrect (vulnerable to path traversal):**
```javascript
const fs = require('fs');

function readUserFile(fileName) {
  fs.readFile(fileName, (err, data) => {
    if (err) throw err;
    console.log(data);
  });
}
```

**Correct (safe with literal path):**
```javascript
const fs = require('fs');

function readConfigFile() {
  fs.readFile('config/settings.json', (err, data) => {
    if (err) throw err;
    console.log(data);
  });
}
```

**References:**
- CWE-22: Path Traversal
- [OWASP Path Traversal](https://owasp.org/www-community/attacks/Path_Traversal)

---

#### path.join/path.resolve Traversal

**Incorrect (vulnerable to path traversal):**
```javascript
const path = require('path');

function getFile(entry) {
  var extractPath = path.join(opts.path, entry.path);
  return extractFile(extractPath);
}
```

**Correct (resolve and enforce boundary):**
```javascript
const path = require('path');

function getFileSafe(req, res) {
  const baseDir = path.resolve(opts.path);
  const resolved = path.resolve(baseDir, '.' + req.body.path);
  if (!resolved.startsWith(baseDir + path.sep)) {
    throw new Error('path traversal attempt');
  }
  return extractFile(resolved);
}
```

**References:**
- CWE-22: Path Traversal
- [OWASP Path Traversal](https://owasp.org/www-community/attacks/Path_Traversal)

---

### Language: Java

#### HttpServlet Path Traversal

**Incorrect (vulnerable to path traversal):**
```java
public class FileServlet extends HttpServlet {
    public void doPost(HttpServletRequest request, HttpServletResponse response)
            throws ServletException, IOException {
        String image = request.getParameter("image");
        File file = new File("static/images/", image);
        if (!file.exists()) {
            response.sendError(404);
        }
    }
}
```

**Correct (sanitized with FilenameUtils):**
```java
public class FileServlet extends HttpServlet {
    public void doPost(HttpServletRequest request, HttpServletResponse response)
            throws ServletException, IOException {
        String image = request.getParameter("image");
        File file = new File("static/images/", FilenameUtils.getName(image));
        if (!file.exists()) {
            response.sendError(404);
        }
    }
}
```

**References:**
- CWE-22: Path Traversal
- [OWASP Path Traversal](https://www.owasp.org/index.php/Path_Traversal)

---

### Language: Go

#### filepath.Clean Misuse

**Incorrect (Clean does not prevent traversal):**
```go
func main() {
    mux := http.NewServeMux()
    mux.HandleFunc("/file", func(w http.ResponseWriter, r *http.Request) {
        filename := filepath.Clean(r.URL.Path)
        filename = filepath.Join(root, strings.Trim(filename, "/"))
        contents, err := ioutil.ReadFile(filename)
        if err != nil {
            w.WriteHeader(http.StatusNotFound)
            return
        }
        w.Write(contents)
    })
}
```

**Correct (prefix with "/" before Clean):**
```go
func main() {
    mux := http.NewServeMux()
    mux.HandleFunc("/file", func(w http.ResponseWriter, r *http.Request) {
        filename := path.Clean("/" + r.URL.Path)
        filename = filepath.Join(root, strings.Trim(filename, "/"))
        contents, err := ioutil.ReadFile(filename)
        if err != nil {
            w.WriteHeader(http.StatusNotFound)
            return
        }
        w.Write(contents)
    })
}
```

**Best Practice:** Use `filepath.FromSlash(path.Clean("/"+strings.Trim(req.URL.Path, "/")))` or the `SecureJoin` function from `github.com/cyphar/filepath-securejoin`.

**References:**
- CWE-22: Path Traversal
- [Go path.Clean Documentation](https://pkg.go.dev/path#Clean)
- [filepath-securejoin Package](https://pkg.go.dev/github.com/cyphar/filepath-securejoin)

---

### Language: PHP

#### File Inclusion (LFI/RFI)

**Incorrect (vulnerable to path traversal/RFI):**
```php
<?php
$user_input = $_GET["page"];
include($user_input);
?>
```

**Correct (constant paths):**
```php
<?php
include('templates/header.php');
require_once(CONFIG_DIR . '/settings.php');
?>
```

**References:**
- CWE-98: PHP Remote File Inclusion
- [PHP include Documentation](https://www.php.net/manual/en/function.include.php)

---

#### unlink() Path Traversal

**Incorrect (vulnerable to path traversal):**
```php
<?php
$data = $_GET["file"];
unlink("/storage/" . $data);
?>
```

**Correct (constant path):**
```php
<?php
unlink('/storage/cache/temp.txt');
?>
```

**References:**
- CWE-22: Path Traversal
- [PHP unlink Documentation](https://www.php.net/manual/en/function.unlink)

---
title: Prevent Command Injection
impact: CRITICAL
impactDescription: Remote code execution allowing attackers to run arbitrary commands on the host system
tags: security, command-injection, cwe-78, cwe-94
---

## Prevent Command Injection

Command injection occurs when untrusted input is passed to system shell commands. Attackers can execute arbitrary commands on the host system, potentially downloading malware, stealing data, or taking complete control of the server.

---

### Language: Python

**Incorrect (vulnerable to command injection via subprocess):**
```python
import subprocess
import flask

app = flask.Flask(__name__)

@app.route("/ping")
def ping():
    ip = flask.request.args.get("ip")
    subprocess.run("ping " + ip, shell=True)
```

**Correct (use array form without shell=True):**
```python
import subprocess
import flask

app = flask.Flask(__name__)

@app.route("/ping")
def ping():
    ip = flask.request.args.get("ip")
    subprocess.run(["ping", ip])
```

---

### Language: JavaScript / Node.js

**Incorrect (vulnerable child_process with user input):**
```javascript
const { exec } = require('child_process');

function runCommand(userInput) {
    exec(`cat ${userInput}`, (error, stdout, stderr) => {
        console.log(stdout);
    });
}
```

**Correct (use spawn with array arguments):**
```javascript
const { spawn } = require('child_process');

function runCommand(userInput) {
    const proc = spawn('cat', [userInput]);
    proc.stdout.on('data', (data) => {
        console.log(data.toString());
    });
}
```

---

### Language: Java

**Incorrect (ProcessBuilder with user input via shell):**
```java
public class CommandRunner {

    public void runCommand(String userInput) throws IOException {
        String[] cmd = {"/bin/bash", "-c", userInput};
        ProcessBuilder builder = new ProcessBuilder(cmd);
        Process proc = builder.start();
    }
}
```

**Correct (use ProcessBuilder with array arguments, no shell):**
```java
public class CommandRunner {

    public void runCommand(String filename) throws IOException {
        ProcessBuilder builder = new ProcessBuilder("cat", filename);
        Process proc = builder.start();
    }
}
```

---

### Language: Go

**Incorrect (dangerous command with user input via stdin):**
```go
import (
    "fmt"
    "os/exec"
)

func runCommand(userInput string) {
    cmd := exec.Command("bash")
    cmdWriter, _ := cmd.StdinPipe()
    cmd.Start()

    cmdString := fmt.Sprintf("echo %s", userInput)
    cmdWriter.Write([]byte(cmdString + "\n"))

    cmd.Wait()
}
```

**Correct (use exec.Command with explicit arguments):**
```go
import (
    "os/exec"
)

func runCommand(filename string) {
    cmd := exec.Command("cat", filename)
    output, _ := cmd.Output()
    println(string(output))
}
```

---

### Language: Ruby

**Incorrect (Shell methods with tainted input):**
```ruby
require 'shell'

def read_file(params)
    Shell.cat(params[:filename])
end
```

**Correct (use hardcoded or validated paths):**
```ruby
require 'shell'

def read_log
    Shell.cat("/var/log/www/access.log")
end
```

---

**References:**
- CWE-78: Improper Neutralization of Special Elements used in an OS Command ('OS Command Injection')
- CWE-94: Improper Control of Generation of Code ('Code Injection')
- [OWASP Command Injection](https://owasp.org/www-community/attacks/Command_Injection)
- [OWASP Top 10 A03:2021 - Injection](https://owasp.org/Top10/A03_2021-Injection)

Before finishing
----------------

1. Does every function do exactly what its name says? Is any name long
   because the function is really several things?
2. Did I extract anything only to save lines? Inline it.
3. Is anything long and tangled that is really two real concepts? Split it.
4. Are preconditions asserted, and does each structure have an invariant
   check that runs on entry and exit?
5. Does a failure leave the caller's data untouched, and does one path
   release everything?
6. Are comments terse, free of "why not the other way" history, and drawn as
   diagrams where a diagram fits?
7. Are the tests next to the code, with failure cases injected?

