---
name: writing-good-tests
description: Write or review tests: grouped trivial tests, parameterized edge cases, simulation tests for concurrency, regression tests, and picking the right test framework for each job.
---

Writing good tests
==================

Use this whenever you write, extend or review tests. It is language neutral:
the examples are pseudo code, so map them onto whatever framework the project
already uses.

There are five kinds of test worth writing. They are not ranked, and most
code needs more than one kind:

1. Trivial tests
2. Parameterized tests
3. Simulation tests
4. Regression tests
5. Picking the right framework for each of the above

Trivial tests
-------------

Trivial tests are necessary. Write one for algorithmic logic, and for anything
a slip of the fingers could break: a loop hitting the right bound, basic
arithmetic propagating up through layers.

But a trivial test should never stand alone. Group trivial checks into one
test using the framework's test cases (subtests, sections, named steps,
whatever it calls them). A pile of one-line tests is noise in the output and
in the file.

Bad:

```text
test foo_bar_is_5:
    assert foo.bar() == 5

test foo_biz_is_10:
    assert foo.biz() == 10
```

Good:

```text
test foo_getters:
    test_case bar_is_5:
        assert foo.bar() == 5
    test_case biz_is_10:
        assert foo.biz() == 10
```

Also fine when the framework has no test cases: put the assertions one after
another in a single test. The point is one test per behavior, not one test per
assertion.

Parameterized tests
-------------------

Encourage parameterized tests with edge conditions. They make it cheap to add
the next edge case: one new row instead of a new test.

When writing a test, ask:

- Is there data this test can be parameterized over?
- Can I find that data, pull the rest of the code out into a shared body, and
  parameterize it?
- Does it need to be parameterized at all?

Parameters must be real parameters. Reading the table should tell you what is
being tested. If the table has to smuggle in behavior (which method to call,
which branch to take), it is confusing. Bad:

```text
parameterized test invalid_query_raises(method_name, args):
    cases:
        ("execute", [])
        ("read",    [])
        ("write",   [[1]])
        ("get",     [])

    # looks the method up by name at runtime
    method = lookup(db, method_name)
    assert_raises error:
        method("not a real query", *args)
```

The reader can't see what is under test without mentally running the
dynamic lookup. Parameterize the data, and call the methods directly.

If every case is already a single line, don't parameterize. Moving a one-line
assertion into a table is just relocating it:

```text
test to_type_mapping:
    assert to_type("u8")  == native_uint8
    assert to_type("u16") == native_uint16
    assert to_type("i8")  == native_int8
    assert to_type("f32") == native_float32
```

Parameterize when each case drives several lines of setup and assertions.
This is good, because every row is data and the body is shared:

```text
parameterized test get_variable(name, type_str, elem_type, dims, size):
    cases:
        ("foo",   "u32",                    uint32,  (),      4)
        # arrays of primitives
        ("foo",   "[10]u32",                uint32,  (10,),   40)
        ("foo",   "[10][20]u32",            uint32,  (10,20), 800)
        # structs
        ("foo",   "struct { a u32, b f32 }", struct1, (),     8)
        # unions
        ("foo",   "union { a u32, b f64 }", union1,  (),      8)
        # names
        ("_x",    "u8",                     uint8,   (),      1)
        ("foo_1", "u8",                     uint8,   (),      1)

    db.create(name, type_str)
    var = db.get(name)
    assert var == Var(name, type_str, 0, size)
    assert var.shape == (0, *dims)
    assert var.elem_type == elem_type
```

### From bad to good

Bad: the same idea copied into four tests.

```text
test invalid_query_raises_execute:
    assert_raises error: db.execute("not a real query")

test invalid_query_raises_read:
    assert_raises error: db.read("not a real query")

test invalid_query_raises_write:
    assert_raises error: db.write("not a real query", [1])

test invalid_query_raises_get:
    assert_raises error: db.get("not a real query")
```

Better: one test, with a test case per call.

```text
test invalid_query_raises:
    assert_raises error: db.execute("not a real query")
    assert_raises error: db.read("not a real query")
    assert_raises error: db.write("not a real query", [1])
    assert_raises error: db.get("not a real query")
```

Best: the varying data pulled out into parameters.

```text
parameterized test invalid_query_raises(query):
    cases:
        "not a real query"
        ""
        "DROP TABLE users;"
        ...

    assert_raises error: db.execute(query)
    assert_raises error: db.read(query)
    assert_raises error: db.write(query, [1])
    assert_raises error: db.get(query)
```

Simulation tests
----------------

Some people call these property based tests, some call them simulation tests.
Either way they are great. The shape:

1. Randomize the inputs to the test.
2. Run a simulation in a loop with a limited action pool, picking actions at
   random to try to break the system with different edge conditions.
3. Finish the simulation and assert the invariants that must always hold.

Design every simulation test so it can run N times. Each run is more
randomness, so running it in a loop only finds more bugs. Always sprinkle
randomization into simulation tests, even alongside hand-picked edge cases.

Always seed the random generator, and print the seed. A failure you can't
replay is nearly useless; with the seed, a failure is a regression test
waiting to be written.

```text
test simulation(seed = random_seed()):
    print("seed:", seed)
    rng = Random(seed)
    model = ReferenceModel()          # a simple, obviously correct version
    system = RealSystem()

    repeat rng.int(100, 10000) times:
        action = rng.choice([insert, remove, read, overwrite, restart])
        args   = rng.generate_args_for(action)
        expected = model.apply(action, args)
        actual   = system.apply(action, args)
        assert actual == expected

    assert_invariants(system)         # nothing lost, nothing duplicated
```

Property based testing goes one step further: instead of repeating the same
test N times, it generates N sets of inputs, and shrinks a failing input down
to a minimal one. That is sometimes hard to set up and not always necessary,
but it is always a plus.

Concurrency
-----------

If anything uses locks or concurrency primitives, it should probably get a
simulation test. Randomized simulation is the best tool for finding races:
initialize some locks, run a lot of threads, give them lots of ways to
collide, then assert the invariants that must survive.

Use a gate so every thread starts at the same instant. Without one, early
threads finish before late threads exist, and the race never happens:

```text
all_ready = CountdownLatch(NTHREADS)
go        = CountdownLatch(1)

worker thread:
    all_ready.count_down()
    go.await()
    run_random_actions()

main:
    start NTHREADS workers
    all_ready.await()        # every worker is parked at the gate
    go.count_down()          # release them together
    join all workers
    assert_invariants()
```

Run concurrency tests many times, and under whatever race or leak detector the
platform offers. One green run proves very little.

Regression tests
----------------

When you find a bug, write a regression test and a detailed description of
what the bug was and what caused it. Future readers will see a test that looks
pointless unless the description says why it exists.

Keep regression tests in their own isolated area of the code, not mixed in
with the rest of the tests. Number or date them so they are easy to refer to.

```text
regression test 0002_create_crash_close_delete:
    clean_state("test")
    db = open("test")

    db.execute("create foo [8][9][3][3] i16")

    db.crash()                       # simulate a crash, then re open
    db = open("test")

    db.close()                       # clean close, then re open
    db = open("test")

    # This failed, and it shouldn't have.
    #
    # CAUSE:
    #   The first log record for the free-space map is an update record, but
    #   that page starts uninitialized. It needs one full physical log record
    #   first before it can be used. Fix: log the physical record, then
    #   continue with the map-specific records.
    db.execute("delete foo")

    db.close()
```

A good regression test is the smallest sequence that reproduces the bug. If a
simulation test found it, keep the seed in the description.

Picking the right framework
---------------------------

Use the right test framework for the right job. Don't bend one tool into
shapes it wasn't built for, and don't hand-roll what the project's framework
already does well.

| Job                          | Use                                          |
| ---------------------------- | -------------------------------------------- |
| Trivial and regression tests | the project's normal unit test framework     |
| Parameterized edge cases     | the framework's parameterization feature     |
| Property based tests         | a property testing library, if one exists    |
| Simulation tests             | a seeded harness, run as its own program     |
| Concurrency stress           | a harness with gates, plus race detectors    |
| Timing and performance       | a benchmark framework, not a unit test       |

Guidelines:

- Prefer what the project already uses. Adding a second unit test framework
  for the same job is almost never worth it.
- Use the framework's own features before writing loops by hand. If you are
  writing a loop over cases, you want parameterization. If you are writing
  setup and teardown by hand in every test, you want fixtures.
- A unit test should be fast and deterministic. A simulation that runs for
  seconds, takes a duration or seed argument, and loops on random actions is
  better as a separate program or a separate test target, so the quick suite
  stays quick. Run the quick suite on every change and the long simulations
  in CI or on a schedule.
- Don't use a unit test framework to benchmark. Wall clock assertions in unit
  tests fail on slow machines for no real reason; use a benchmark tool that
  does warmup and statistics.
- If the property testing library can't do what you need (custom models,
  crashes, threads), a small hand-rolled seeded harness is fine. Keep it
  simple, print the seed, and make it exit non-zero on failure so CI notices.
- Whatever the tool, a test failure should name the case that failed and the
  inputs, and a simulation failure should print its seed.

Before finishing
----------------

1. Is any test a one-line behavior sitting alone? Group it.
2. Are there several tests that differ only in data? Parameterize them.
3. Is any parameter table hard to read, or does it hide behavior? Simplify.
4. Does anything use locks or threads without a simulation test?
5. Does every simulation use a seed and print it?
6. Does every fixed bug have a regression test with a written cause?
7. Is each test in the right tool, and does the fast suite still run fast?
