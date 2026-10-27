Errors, cleanup and resources
=============================

Read this when you write code that can fail, or code that acquires, moves or
releases resources.

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
