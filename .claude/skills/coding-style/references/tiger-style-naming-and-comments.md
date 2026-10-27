Tiger Style: naming, ordering and commit messages
=================================================

Adapted from TigerBeetle's Tiger Style guide. The examples are Zig, but most
of the principles are general. Read this when you choose names, order a file,
or write a commit message.

This file conflicts with the rest of the skill in three places. SKILL.md and
`comments-and-tests.md` win:

- Comments. Tiger Style asks for full-sentence comments and for saying why.
  This skill asks for terse, mostly visual comments with no "why not the other
  way" history. See `comments-and-tests.md`. The commit message guidance below
  does not conflict, and still applies.
- Abbreviations. Tiger Style says don't abbreviate variable names. Short
  module prefixes on public functions (`wal_`, `pgr_`) are still right here.
  See `design-naming-concurrency.md`.
- Case. `snake_case` below is Zig's convention. Follow the language and the
  surrounding code.

Names
-----

- Get the nouns and verbs just right. Great names are the essence of great
  code, they capture what a thing is or does, and provide a crisp, intuitive
  mental model. They show that you understand the domain. Take time to find
  the perfect name, to find nouns and verbs that work together, so that the
  whole is greater than the sum of its parts.
- Use `snake_case` for function, variable, and file names. The underscore is
  the closest thing we have as programmers to a space, and helps to separate
  words and encourage descriptive names. We don't use Zig's `CamelCase.zig`
  style for "struct" files to keep the convention simple and consistent.
- Do not abbreviate variable names, unless the variable is a primitive integer
  type used as an argument to a sort function or matrix calculation. Use long
  form arguments in scripts: `--force`, not `-f`. Single letter flags are for
  interactive usage.
- Use proper capitalization for acronyms (`VSRState`, not `VsrState`).
- For the rest, follow the Zig style guide.
- Add units or qualifiers to variable names, and put the units or qualifiers
  last, sorted by descending significance, so that the variable starts with
  the most significant word, and ends with the least significant word. For
  example, `latency_ms_max` rather than `max_latency_ms`. This will then line
  up nicely when `latency_ms_min` is added, as well as group all variables
  that relate to latency.
- Infuse names with meaning. For example, `allocator: Allocator` is a good, if
  boring name, but `gpa: Allocator` and `arena: Allocator` are excellent. They
  inform the reader whether `deinit` should be called explicitly.
- When choosing related names, try hard to find names with the same number of
  characters so that related variables all line up in the source. For example,
  as arguments to a memcpy function, `source` and `target` are better than
  `src` and `dest` because they have the second-order effect that any related
  variables such as `source_offset` and `target_offset` will all line up in
  calculations and slices. This makes the code symmetrical, with clean blocks
  that are easier for the eye to parse and for the reader to check.
- When a single function calls out to a helper function or callback, prefix
  the name of the helper function with the name of the calling function to
  show the call history. For example, `read_sector()` and
  `read_sector_callback()`.
- Callbacks go last in the list of parameters. This mirrors control flow:
  callbacks are also invoked last.
- Don't overload names with multiple meanings that are context-dependent. For
  example, TigerBeetle has a feature called pending transfers where a pending
  transfer can be subsequently posted or voided. At first, we called them
  two-phase commit transfers, but this overloaded the two-phase commit
  terminology that was used in our consensus protocol, causing confusion.
- Think of how names will be used outside the code, in documentation or
  communication. For example, a noun is often a better descriptor than an
  adjective or present participle, because a noun can be directly used in
  correspondence without having to be rephrased. Compare `replica.pipeline`
  vs `replica.preparing`. The former can be used directly as a section header
  in a document or conversation, whereas the latter must be clarified. Noun
  names compose more clearly for derived identifiers, e.g.
  `config.pipeline_max`.
- Zig has named arguments through the `options: struct` pattern. Use it when
  arguments can be mixed up. A function taking two `u64` must use an options
  struct. If an argument can be `null`, it should be named so that the meaning
  of `null` literal at the call site is clear.

  Because dependencies like an allocator or a tracer are singletons with
  unique types, they should be threaded through constructors positionally,
  from the most general to the most specific.

Order
-----

Order matters for readability (even if it doesn't affect semantics). On the
first read, a file is read top-down, so put important things near the top. The
`main` function goes first.

The same goes for structs: the order is fields, then types, then methods:

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

At the same time, not everything has a single right order. When in doubt,
consider sorting alphabetically, taking advantage of big-endian naming.

Commit messages
---------------

- Write descriptive commit messages that inform and delight the reader,
  because your commit messages are being read. Note that a pull request
  description is not stored in the git repository and is invisible in
  `git blame`, and therefore is not a replacement for a commit message.

Comments (Tiger Style)
----------------------

Kept for completeness. Where these differ from `comments-and-tests.md`, that
file wins.

- Don't forget to say why. Code alone is not documentation. Use comments to
  explain why you wrote the code the way you did. Show your workings.
- Don't forget to say how. For example, when writing a test, think of writing
  a description at the top to explain the goal and methodology of the test, to
  help your reader get up to speed, or to skip over sections, without forcing
  them to dive in.
- Comments are sentences, with a space after the slash, with a capital letter
  and a full stop, or a colon if they relate to something that follows.
  Comments are well-written prose describing the code, not just scribblings in
  the margin. Comments after the end of a line can be phrases, with no
  punctuation.
