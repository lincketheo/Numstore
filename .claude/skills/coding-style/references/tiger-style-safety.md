Tiger Style: safety, scope and control flow
===========================================

Adapted from TigerBeetle's Tiger Style guide. The examples are Zig, but the
principles are general. Where this file disagrees with SKILL.md or the other
references, SKILL.md wins. See also `assertions.md`.

Read this when you write low level or correctness critical code: bounds,
assertions, variable scope, control flow, and how functions are shaped.

Limits and types
----------------

- Put a limit on everything, because, in reality, this is what we expect:
  everything has a limit. For example, all loops and all queues must have a
  fixed upper bound to prevent infinite loops or tail latency spikes. This
  follows the ["fail-fast"](https://en.wikipedia.org/wiki/Fail-fast)
  principle so that violations are detected sooner rather than later. Where a
  loop cannot terminate (e.g. an event loop), this must be asserted.
- Use explicitly-sized types like `u32` for everything, avoid
  architecture-specific `usize`.

Assertions
----------

Assertions detect programmer errors. Unlike operating errors, which are
expected and which must be handled, assertion failures are unexpected. The
only correct way to handle corrupt code is to crash. Assertions downgrade
catastrophic correctness bugs into liveness bugs. Assertions are a force
multiplier for discovering bugs by fuzzing.

- Assert all function arguments and return values, pre/postconditions and
  invariants. A function must not operate blindly on data it has not checked.
  The purpose of a function is to increase the probability that a program is
  correct. Assertions within a function are part of how functions serve this
  purpose. The assertion density of the code must average a minimum of two
  assertions per function.
- [Pair assertions](https://tigerbeetle.com/blog/2023-12-27-it-takes-two-to-contract).
  For every property you want to enforce, try to find at least two different
  code paths where an assertion can be added. For example, assert validity of
  data right before writing it to disk, and also immediately after reading
  from disk.
- On occasion, you may use a blatantly true assertion instead of a comment as
  stronger documentation where the assertion condition is critical and
  surprising.
- Split compound assertions: prefer `assert(a); assert(b);` over
  `assert(a and b);`. The former is simpler to read, and provides more precise
  information if the condition fails.
- Use single-line `if` to assert an implication: `if (a) assert(b)`.
- Assert the relationships of compile-time constants as a sanity check, and
  also to document and enforce
  [subtle invariants](https://github.com/coilhq/tigerbeetle/blob/db789acfb93584e5cb9f331f9d6092ef90b53ea6/src/vsr/journal.zig#L45-L47)
  or [type sizes](https://github.com/coilhq/tigerbeetle/blob/578ac603326e1d3d33532701cb9285d5d2532fe7/src/ewah.zig#L41-L53).
  Compile-time assertions are extremely powerful because they are able to
  check a program's design integrity before the program even executes.
- The golden rule of assertions is to assert the positive space that you do
  expect AND to assert the negative space that you do not expect, because
  where data moves across the valid/invalid boundary between these spaces is
  where interesting bugs are often found. This is also why tests must test
  exhaustively, not only with valid data but also with invalid data, and as
  valid data becomes invalid.
- Assertions are a safety net, not a substitute for human understanding. With
  simulation testing, there is the temptation to trust the fuzzer. But a
  fuzzer can prove only the presence of bugs, not their absence. Therefore:
  - Build a precise mental model of the code first,
  - encode your understanding in the form of assertions,
  - write the code and comments to explain and justify the mental model to
    your reviewer,
  - and use VOPR (TigerBeetle's simulator) as the final line of defense, to
    find bugs in your and the reviewer's understanding of the code.

Scope and state
---------------

- Declare variables at the smallest possible scope, and minimize the number of
  variables in scope, to reduce the probability that variables are misused.
- Shrink the scope to minimize the number of variables at play and reduce the
  probability that the wrong variable is used.
- Calculate or check variables close to where/when they are used. Don't
  introduce variables before they are needed. Don't leave them around where
  they are not. This will reduce the probability of a POCPOU
  (place-of-check to place-of-use), a distant cousin to the infamous
  [TOCTOU](https://en.wikipedia.org/wiki/Time-of-check_to_time-of-use). Most
  bugs come down to a semantic gap, caused by a gap in time or space, because
  it's harder to check code that's not contained along those dimensions.
- Don't duplicate variables or take aliases to them. This will reduce the
  probability that state gets out of sync.
- Use newlines to group resource allocation and deallocation, i.e. before the
  resource allocation and after the corresponding `defer` statement, to make
  leaks easier to spot.

Function shape and control flow
-------------------------------

- Good function shape is often the inverse of an hourglass: a few parameters,
  a simple return type, and a lot of meaty logic between the braces.
- Centralize control flow. When splitting a large function, try to keep all
  switch/if statements in the "parent" function, and move non-branchy logic
  fragments to helper functions. Divide responsibility. All control flow
  should be handled by one function, the rest shouldn't care about control
  flow at all. In other words,
  ["push `if`s up and `for`s down"](https://matklad.github.io/2023/11/15/push-ifs-up-and-fors-down.html).
- Similarly, centralize state manipulation. Let the parent function keep all
  relevant state in local variables, and use helpers to compute what needs to
  change, rather than applying the change directly. Keep leaf functions pure.
- Compound conditions that evaluate multiple booleans make it difficult for
  the reader to verify that all cases are handled. Split compound conditions
  into simple conditions using nested `if/else` branches. Split complex
  `else if` chains into `else { if { } }` trees. This makes the branches and
  cases clear. Again, consider whether a single `if` does not also need a
  matching `else` branch, to ensure that the positive and negative spaces are
  handled or asserted.
- Negations are not easy! State invariants positively. When working with
  lengths and indexes, this form is easy to get right (and understand):

  ```zig
  if (index < count) {
    // The invariant holds.
  } else {
    // The invariant doesn't hold.
  }
  ```

  This form is harder, and also goes against the grain of how `index` would
  typically be compared to `count`, for example, in a loop condition:

  ```zig
  if (index >= count) {
    // It's not true that the invariant holds.
  }
  ```

- Use simpler function signatures and return types to reduce dimensionality
  at the call site, the number of branches that need to be handled at the call
  site, because this dimensionality can also be viral, propagating through the
  call chain. For example, as a return type, `void` trumps `bool`, `bool`
  trumps `u64`, `u64` trumps `?u64`, and `?u64` trumps `!u64`.
- Ensure that functions run to completion without suspending, so that
  precondition assertions are true throughout the lifetime of the function.
  These assertions are useful documentation without a suspend, but may be
  misleading otherwise.
- Explicitly pass options to library functions at the call site, instead of
  relying on the defaults. For example, write
  `@prefetch(a, .{ .cache = .data, .rw = .read, .locality = 3 });` over
  `@prefetch(a, .{});`. This improves readability but most of all avoids
  latent, potentially catastrophic bugs in case the library ever changes its
  defaults.

Memory
------

- If you don't mean a function argument to be copied when passed by value, and
  if the argument type is more than 16 bytes, then pass the argument as
  `*const`. This will catch bugs where the caller makes an accidental copy on
  the stack before calling the function.
- Construct larger structs in-place by passing an out pointer during
  initialization.

  In-place initializations can assume pointer stability and immovable types
  while eliminating intermediate copy-move allocations, which can lead to
  undesirable stack growth.

  Keep in mind that in-place initializations are viral: if any field is
  initialized in-place, the entire container struct should be initialized
  in-place as well.

  Prefer:

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

  Over:

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

- Be on your guard for
  [buffer bleeds](https://en.wikipedia.org/wiki/Heartbleed). This is a buffer
  underflow, the opposite of a buffer overflow, where a buffer is not fully
  utilized, with padding not zeroed correctly. This may not only leak
  sensitive information, but may cause deterministic guarantees as required by
  TigerBeetle to be violated.
