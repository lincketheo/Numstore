Simplicity, functions and intent
================================

Read this when you write, split, merge or review functions. The most important
rule in the whole skill is here: functions capture intent.

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
