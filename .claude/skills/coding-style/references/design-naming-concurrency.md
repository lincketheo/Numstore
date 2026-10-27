Interfaces, naming and concurrency
==================================

Read this when you design a module's interface, name things, or write code
that takes locks or uses threads. For more naming rules, see
`tiger-style-naming-and-comments.md`.

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
