---
name: coding-style
description: Conventions for writing, refactoring and reviewing code in any language: simple code, function intent and locality, assertions, failure handling, terse visual comments, tests beside code; with security and performance references.
---

Coding style and conventions
============================

Use this whenever you write, extend, refactor or review code. It is about how
code is structured and what it means, not how it looks. The principles apply
to any language; the examples are pseudo code, with a few C notes because the
reference code was C.

Not covered here, on purpose: formatting, whitespace, file headers and banner
comments. A formatter and the surrounding code own those. Match what is
already in the file.

The core ideas, in priority order:

1. Functions capture intent. This is the most important rule.
2. Code is simple and straightforward, and reads top to bottom.
3. The state of the program is asserted, not assumed.
4. Failure is handled on purpose, and leaves things as they were.
5. Comments are terse and visual. Tests sit next to the code they test.

How to use this skill
---------------------

Read this file first, then open only the references that match the task. They
are in the `references/` folder next to this file.

| File                                            | Read it when                              |
| ----------------------------------------------- | ----------------------------------------- |
| `references/working-approach.md`                | starting any task; before editing code    |
| `references/functions-and-intent.md`            | writing, splitting or reviewing functions |
| `references/assertions.md`                      | adding checks, invariants, preconditions  |
| `references/errors-and-resources.md`            | failure paths, cleanup, ownership         |
| `references/design-naming-concurrency.md`       | interfaces, naming, locks and threads     |
| `references/comments-and-tests.md`              | writing comments, or tests for the code   |
| `references/c-notes.md`                         | the code is C                             |
| `references/tiger-style-safety.md`              | limits, scope, control flow, memory       |
| `references/tiger-style-naming-and-comments.md` | naming details, ordering, commits         |
| `references/tiger-style-performance.md`         | design-time performance thinking          |
| `references/security-memory-safety.md`          | C/C++ memory handling                     |
| `references/security-unsafe-functions.md`       | banned or risky library functions         |
| `references/security-path-traversal.md`         | paths built from outside input            |
| `references/security-command-injection.md`      | running commands or shells                |

Always read `working-approach.md` at the start of a task. It covers how to
approach the change itself: surface assumptions, keep it minimal, touch only
what was asked, and define how you will verify it.

When references disagree
------------------------

1. The core ideas above come first.
2. Then the author's own references: `working-approach.md`,
   `functions-and-intent.md`, `assertions.md`, `errors-and-resources.md`,
   `design-naming-concurrency.md`, `comments-and-tests.md` and `c-notes.md`.
3. Then the `tiger-style-*` references, which are adapted from another
   project's guide and are supplementary. Where they differ, the earlier
   references win. The known differences are listed at the top of
   `tiger-style-naming-and-comments.md`.
4. The `security-*` references are rule sets for specific hazards. They don't
   conflict with style. Follow them whenever the code touches that hazard.

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
8. Does every changed line trace back to the request, and did I verify the
   result with a check I defined up front?
9. Does the code touch untrusted input, files, processes or raw memory? If so,
   did I read the matching `security-*` reference?
