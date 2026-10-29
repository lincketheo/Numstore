Python API Reference
====================

Every public name in `pynumstore`, with a runnable example for each. If you are
new to Numstore, read [the tour](index.md) first - it explains the query
language this page assumes. For the C library, see [the C API](c_api.md).

Everything below is a real Python session. Examples run in order and share one
database.

* [Install](#install)
* [Database](#database)
* [Queries](#queries)
* [Transactions](#transactions)
* [Concurrency and threading](#concurrency-and-threading)
* [Durability](#durability)
* [Plan](#plan)
* [Var](#var)
* [to_dtype](#to_dtype)
* [Errors](#errors)
* [Version](#version)

Install
-------

```sh
pip install pynumstore
```

Numpy is the only dependency, and it comes with the wheel. One complete
round trip - create a variable, insert data, read it back:

```python
>>> import numpy as np
>>> import pynumstore as ns
>>> with ns.Database("quickstart.db") as db:
...     db.execute("create prices f64")
...     db.write("insert prices 0 3", np.array([1.5, 2.25, 3.75]))
...     db.read("read prices[0:]")
3
array([1.5 , 2.25, 3.75])
```

Database
--------

`ns.Database(path)` opens the database file at `path`, creating it if it isn't
there. A database is a single file and there is no server to start.

```python
>>> db = ns.Database("api.db")
>>> db
<Database 'api.db'>
>>> db.path
'api.db'
>>> db.closed
False
```

| Member               | Type                      | What it does                                      |
|----------------------|---------------------------|---------------------------------------------------|
| `Database(path)`     | `str \| os.PathLike`      | opens (or creates) the database file              |
| `db.path`            | `str`                     | the path it was opened with                       |
| `db.closed`          | `bool`                    | `True` once `close()` has run                     |
| `db.close()`         | `None`                    | flushes and closes; safe to call twice            |
| `db.begin()`         | `Transaction`             | opens a transaction - see [Transactions](#transactions) |
| `db.prepare(query)`  | `Plan`                    | compiles a query for reuse - see [Plan](#plan)    |
| `db.execute/get/get_all/read/read_into/write` | | the query methods - see [Queries](#queries) |

`path` accepts anything `os.PathLike`, so `pathlib.Path` works:

```python
>>> from pathlib import Path
>>> tmp = ns.Database(Path("from_path.db"))
>>> tmp.path
'from_path.db'
>>> tmp.close()
```

`Database` is a context manager, which is how you should normally open one. The
block closes it even if the body raises:

```python
>>> with ns.Database("scratch.db") as scratch:
...     scratch.execute("create tmp u8")
...     scratch.closed
False
>>> scratch.closed
True
```

Closing twice is a no-op, but using a closed database is an error rather than a
crash:

```python
>>> scratch.close()
>>> scratch.read("read tmp")
Traceback (most recent call last):
  ...
RuntimeError: database is closed
```

Queries
-------

A query is just a string. Which method you hand it to depends on what has to
cross the boundary - nothing, metadata, data in, or data out. `Database`,
`Transaction` and `Plan` all expose the same set.

| Method               | Queries it accepts         | Returns                        |
|----------------------|----------------------------|--------------------------------|
| `db.execute(q)`      | `CREATE` `DELETE` `REMOVE` | `None`                         |
| `db.get(q)`          | anything naming a variable | a [`Var`](#var)                |
| `db.get_all()`       | takes no query             | `list[Var]`, one per variable  |
| `db.write(q, data)`  | `INSERT` `WRITE`           | `int` - elements written       |
| `db.read(q)`         | `READ` `REMOVE`            | a fresh `numpy` array          |
| `db.read_into(q, o)` | `READ` `REMOVE`            | `int` - elements read into `o` |

### execute

`execute(query) -> None` runs a query that moves no data. That means `CREATE`,
`DELETE`, and `REMOVE` when you don't want the removed data back:

```python
>>> db.execute("create v u32")
>>> db.write("insert v 0 6", [1, 2, 3, 4, 5, 6])
6
>>> db.execute("remove v[0:2]")
>>> db.read("read v")
array([3, 4, 5, 6], dtype=uint32)
>>> db.execute("delete v")
```

Handing it a query that needs a buffer is an error, not a surprise:

```python
>>> db.execute("read v")
Traceback (most recent call last):
  ...
RuntimeError: Only supported exec commands are REMOVE/CREATE/DELETE
```

### write

`write(query, data) -> int` sends data in and returns how many elements landed.
It takes `INSERT` and `WRITE`. `data` is anything numpy can turn into an array -
a list, a tuple, or an array:

```python
>>> db.execute("create prices f64")
>>> db.write("insert prices 0 2", [1.5, 2.5])
2
>>> db.write("insert prices 2 2", (3.5, 4.5))
2
>>> db.write("insert prices 4 2", np.array([5.5, 6.5]))
2
>>> db.read("read prices")
array([1.5, 2.5, 3.5, 4.5, 5.5, 6.5])
```

The count in the query and the length of `data` don't have to agree: the pair is
walked together and stops when either runs out, which is why the return value is
worth reading.

When you pass a numpy array its dtype is checked and only a same-kind cast is
allowed, so a lossy write is refused instead of silently truncating:

```python
>>> db.execute("create counts u32")
>>> db.write("insert counts 0 2", np.array([1.5, 2.5]))
Traceback (most recent call last):
  ...
TypeError: cannot write float64 data to a uint32 variable
```

A plain list gets no such protection - it goes through `np.asarray(data, dtype)`
and casts however numpy sees fit. Pass an array when you care:

```python
>>> db.write("insert counts 0 2", [1.5, 2.5])
2
>>> db.read("read counts")
array([1, 2], dtype=uint32)
```

### read

`read(query) -> np.ndarray` allocates. It asks the variable for its dtype and
shape, allocates exactly that, and hands it to you. It takes `READ`, and
`REMOVE` when you want the removed data back:

```python
>>> db.read("read prices[0:3]")
array([1.5, 2.5, 3.5])
>>> db.read("remove prices[0:2]")
array([1.5, 2.5])
>>> db.read("read prices")
array([3.5, 4.5, 5.5, 6.5])
```

An empty variable reads back as an empty array. An empty *range* of a non-empty
variable has nothing to allocate and raises - use `read_into` if you want
"whatever is there, possibly nothing":

```python
>>> db.execute("create empty u32")
>>> db.read("read empty")
array([], dtype=uint32)
>>> db.read("read prices[9:]")
Traceback (most recent call last):
  ...
RuntimeError: numstore operation failed
```

### read_into

`read_into(query, out) -> int` doesn't allocate. You bring the buffer, it fills
as much as it can and returns how many elements it wrote. That makes it the tool
for reading a large variable in chunks, or for reading into memory you already
own:

```python
>>> buf = np.zeros(2, dtype=np.float64)
>>> db.read_into("read prices", buf)
2
>>> buf
array([3.5, 4.5])
```

The buffer must match the variable's dtype exactly, be C-contiguous, and be
writable - no conversion happens, because the point is to write straight into
your memory:

```python
>>> db.read_into("read prices", np.zeros(4, dtype=np.uint32))
Traceback (most recent call last):
  ...
TypeError: out has dtype uint32, expected float64
>>> db.read_into("read prices", [0, 0, 0, 0])
Traceback (most recent call last):
  ...
TypeError: out must be a numpy array, not list
>>> db.read_into("read prices", np.zeros(8, dtype=np.float64)[::2])
Traceback (most recent call last):
  ...
ValueError: out must be C-contiguous and writable
```

Reading a whole variable in fixed size chunks:

```python
>>> db.execute("create stream u32")
>>> db.write("insert stream 0 10", np.arange(10, dtype=np.uint32))
10
>>> chunk = np.zeros(4, dtype=np.uint32)
>>> total, start = 0, 0
>>> while True:
...     n = db.read_into(f"read stream[{start}:{start + 4}]", chunk)
...     if n == 0:
...         break
...     total += n
...     start += n
>>> total
10
```

### get and get_all

`get(query) -> Var` returns a snapshot of a variable's metadata. Any query that
names a variable works, and the query itself never runs:

```python
>>> db.get("get prices")
Var(name='prices', type='f64', length=4, tsize=8)
>>> db.get("remove prices[0:]")      # prices is not modified
Var(name='prices', type='f64', length=4, tsize=8)
```

Asking about a variable that isn't there is an error:

```python
>>> db.get("get nope")
Traceback (most recent call last):
  ...
RuntimeError: Variable: nope doesn't exist
```

`get_all() -> list[Var]` takes no query and returns one `Var` for every variable
in the database. The order isn't specified, so sort if you need a stable list:

```python
>>> sorted(v.name for v in db.get_all())
['counts', 'empty', 'prices', 'stream']
>>> {v.name: v.length for v in db.get_all()}["stream"]
10
```

Transactions
------------

Every query above ran on its own. A query that isn't given a transaction gets an
automatic one: it either happens completely or not at all, and it is durable by
the time the call returns.

`db.begin() -> Transaction` groups several queries into one atomic unit. A
`Transaction` has the same query methods as a `Database`, and reads on it see
its own uncommitted work:

```python
>>> txn = db.begin()
>>> txn
<Transaction open>
>>> txn.closed
False
>>> txn.write("insert prices 0 1", [0.5])
1
>>> txn.read("read prices")
array([0.5, 3.5, 4.5, 5.5, 6.5])
>>> txn.commit()
>>> db.read("read prices")
array([0.5, 3.5, 4.5, 5.5, 6.5])
```

| Member            | Returns  | What it does                                     |
|-------------------|----------|--------------------------------------------------|
| `txn.commit()`    | `None`   | makes the work durable and closes the transaction |
| `txn.rollback()`  | `None`   | throws the work away and closes the transaction   |
| `txn.closed`      | `bool`   | `True` after commit or rollback                   |
| `txn.execute/get/get_all/read/read_into/write` | | the same query methods, inside the transaction |

`rollback` undoes everything the transaction did:

```python
>>> txn = db.begin()
>>> txn.write("insert prices 0 2", [98.0, 99.0])
2
>>> txn.rollback()
>>> db.read("read prices")
array([0.5, 3.5, 4.5, 5.5, 6.5])
```

A transaction is finished once you commit or roll it back. Using it again is an
error rather than a silent no-op:

```python
>>> txn
<Transaction closed>
>>> txn.commit()
Traceback (most recent call last):
  ...
RuntimeError: transaction was already committed or rolled back
```

In practice you want the context manager, which commits when the block finishes
and rolls back if it raises:

```python
>>> with db.begin() as txn:
...     txn.write("insert prices 5 1", [7.5])
1
>>> db.read("read prices")
array([0.5, 3.5, 4.5, 5.5, 6.5, 7.5])
>>> try:
...     with db.begin() as txn:
...         txn.execute("remove prices")
...         raise ValueError("changed my mind")
... except ValueError as exc:
...     print(exc)
changed my mind
>>> db.read("read prices")
array([0.5, 3.5, 4.5, 5.5, 6.5, 7.5])
```

Concurrency and threading
-------------------------

Numstore serializes transactions strictly: **one transaction is open at a time
per database handle**, and `begin()` waits for the open one to finish before it
returns. That is what makes transactions strictly serializable - the schedule is
the order in which they commit, with no interleaving to reason about.

The practical consequence today is that you must finish a transaction before
starting the next one on the same handle. Open a second one without closing the
first and `begin()` blocks forever waiting for a transaction only you can
finish:

```python
# Don't do this: the second begin() waits for the first, which never commits.
txn1 = db.begin()
txn2 = db.begin()   # blocks
```

The same applies to two threads sharing one `Database`: the waiting `begin()`
does not currently release the GIL, so the thread holding the open transaction
never gets to commit. One transaction at a time, start to finish, is the
supported pattern:

```python
# Fine: each transaction is finished before the next one starts.
for batch in batches:
    with db.begin() as txn:
        txn.write("insert stream 0 %d" % len(batch), batch)
```

Opening the same file twice is also not a way around this. Two handles on one
file both open, but the second does not see the first's commits:

```python
>>> a = ns.Database("shared.db")
>>> a.execute("create v u32")
>>> a.write("insert v 0 2", [1, 2])
2
>>> b = ns.Database("shared.db")      # opens, but is not coherent with a
>>> a.write("insert v 2 1", [3])
1
>>> b.read("read v")                  # a's third element is missing
array([1, 2], dtype=uint32)
>>> a.close(); b.close()
```

Open one `Database` per file, per process, and use it serially. Concurrent
readers and writers across threads and processes are in development; until then
treat a database file as owned by one handle at a time.

Durability
----------

Three facts the API guarantees:

* `commit()` is the durability point. When it returns, the data is on disk.
* A query with no transaction gets an automatic one, so it is durable by the
  time the call returns too.
* `close()` flushes and leaves nothing to recover, but it is **not** what makes
  committed data durable - `commit()` already did that. A process killed between
  `commit()` and `close()` loses nothing.

Recovery is automatic: `ns.Database(path)` replays the log before your first
query runs. For the mechanism, the crash walkthrough and what the files on disk
look like, see
[Durability and crash recovery](index.md#durability-and-crash-recovery).

Plan
----

Every call above compiled its query string, ran it, and threw the compiled form
away. `db.prepare(query) -> Plan` keeps it, so a query you run in a loop is
compiled once:

```python
>>> plan = db.prepare("read prices[0:2]")
>>> plan
<Plan 'read prices[0:2]'>
>>> plan.query
'read prices[0:2]'
>>> plan.read()
array([0.5, 3.5])
>>> plan.read()
array([0.5, 3.5])
>>> plan.close()
>>> plan.closed
True
```

| Member                      | Returns        | What it does                              |
|-----------------------------|----------------|-------------------------------------------|
| `plan.query`                | `str`          | the query it was compiled from            |
| `plan.closed`               | `bool`         | `True` once `close()` has run             |
| `plan.execute(txn=None)`    | `None`         | like `db.execute`                         |
| `plan.read(txn=None)`       | `np.ndarray`   | like `db.read`                            |
| `plan.read_into(out, txn=None)` | `int`      | like `db.read_into`                       |
| `plan.write(data, txn=None)`| `int`          | like `db.write`                           |
| `plan.var(txn=None)`        | `Var`          | the variable the query names              |
| `plan.close()`              | `None`         | releases it; safe to call twice           |

Each method takes an optional transaction as its last argument, so one compiled
plan can run inside any transaction. `Plan` is a context manager too:

```python
>>> with db.prepare("insert stream 0 2") as insert:
...     with db.begin() as txn:
...         insert.write([100, 200], txn)
2
>>> db.read("read stream[0:4]")
array([100, 200,   0,   1], dtype=uint32)
```

Compiling checks the syntax of the query, so a bad query fails at `prepare`
rather than later:

```python
>>> db.prepare("not a query")
Traceback (most recent call last):
  ...
RuntimeError: Expected a valid operation at pos: 0
```

The variable, though, is resolved when the plan runs, not when it compiles. A
plan for a variable that doesn't exist yet compiles fine and fails on use, which
is what lets you prepare against a variable you are about to create:

```python
>>> later = db.prepare("read not_yet[0:]")
>>> later.read()
Traceback (most recent call last):
  ...
RuntimeError: Variable: not_yet doesn't exist
>>> later.close()
```

A plan belongs to the database that made it, and a transaction from a different
database is refused:

```python
>>> other = ns.Database("other.db")
>>> other_txn = other.begin()
>>> db.prepare("read prices").read(other_txn)
Traceback (most recent call last):
  ...
ValueError: transaction belongs to a different database
>>> other_txn.rollback()
>>> other.close()
```

Var
---

`Var` is a frozen dataclass: a snapshot of a variable's metadata, taken when you
asked. It keeps reporting what was true then, so re-read it after you change the
variable.

```python
>>> var = db.get("get prices")
>>> var.name, var.type
('prices', 'f64')
>>> var.length, var.tsize
(6, 8)
>>> var.dtype
dtype('float64')
>>> var.shape
(6,)
>>> len(var)
6
```

| Field / property | Type                 | What it is                                       |
|------------------|----------------------|--------------------------------------------------|
| `var.name`       | `str`                | the variable's name                              |
| `var.type`       | `str`                | its Numstore type, as text                       |
| `var.length`     | `int`                | number of elements when the snapshot was taken   |
| `var.tsize`      | `int`                | size of **one** element, in bytes                |
| `var.dtype`      | `np.dtype`           | the numpy dtype one element maps to              |
| `var.shape`      | `tuple[int, ...]`    | the shape an array of the whole variable has     |
| `len(var)`       | `int`                | same as `var.length`                             |

For a strict array type, `tsize` covers the whole element and `shape` picks up
the trailing dimensions:

```python
>>> db.execute("create grid [2][3] u8")
>>> db.write("insert grid 0 4", np.zeros((4, 2, 3), dtype=np.uint8))
4
>>> grid = db.get("get grid")
>>> grid.tsize, grid.dtype, grid.shape
(6, dtype('uint8'), (4, 2, 3))
```

to_dtype
--------

`ns.to_dtype(type_str) -> np.dtype` maps a Numstore type to the numpy dtype one
element becomes, without touching a database. It is the quickest way to check
what a type costs:

```python
>>> ns.to_dtype("u32")
dtype('uint32')
>>> ns.to_dtype("struct { a u16, b i64, c f32 }").itemsize
14
>>> ns.to_dtype("[10][20] u32").itemsize
800
>>> ns.to_dtype("union { a u32, b cf128 }")
dtype({'names': ['a', 'b'], 'formats': ['<u4', '<c16'], 'offsets': [0, 0], 'itemsize': 16})
```

A type numpy can't represent raises rather than guessing. `f128` and `cf256`
need a 16 byte float, which numpy only has where the platform's `long double` is
16 bytes wide:

```python
>>> ns.to_dtype("struct { i u8, i u32 }")
Traceback (most recent call last):
  ...
ValueError: duplicate key: i
```

```python
>>> try:
...     ns.to_dtype("f128")
... except NotImplementedError as exc:
...     print("not representable here")
... else:
...     print("16 byte long double")  # doctest: +SKIP
not representable here
```

The database itself stores those types anywhere - only the numpy mapping is
platform dependent, so `get` keeps working where `read` and `write` cannot:

```python
>>> db.execute("create wide f128")
>>> db.get("get wide").tsize
16
```

Errors
------

Numstore reports failures as standard Python exceptions. There is no custom
exception hierarchy to import.

| Exception              | When                                                     |
|------------------------|----------------------------------------------------------|
| `RuntimeError`         | the database or the query layer refused: a missing variable, a query handed to the wrong method, a syntax error, a closed database, plan or transaction |
| `TypeError`            | the data or buffer has the wrong dtype or the wrong Python type |
| `ValueError`           | the shape doesn't fit, a type is malformed, or a transaction is from another database |
| `NotImplementedError`  | the type has no numpy representation on this platform (`f128`, `cf256`) |

The messages are written to be read, so logging the exception is usually enough:

```python
>>> try:
...     db.write("insert counts 0 1", np.array([3.5]))
... except TypeError as exc:
...     print(f"refused: {exc}")
refused: cannot write float64 data to a uint32 variable
```

Because a failed query is rolled back, catching an error inside a transaction
leaves the transaction usable - and if you let the error escape the `with`
block, the whole transaction is rolled back:

```python
>>> with db.begin() as txn:
...     try:
...         txn.write("insert counts 0 1", np.array([3.5]))
...     except TypeError:
...         txn.write("insert counts 0 1", np.array([3], dtype=np.uint32))
1
>>> db.read("read counts")
array([3, 1, 2], dtype=uint32)
```

Version
-------

```python
>>> ns.__version__          # doctest: +SKIP
'1.5.0'
>>> isinstance(ns.__version__, str)
True
```

`pynumstore` ships type hints (`py.typed`), so editors and type checkers see
every signature on this page without a stub package.

```python
>>> db.close()
```
