Table of Contents
=================

* [What is Numstore?](#what-is-numstore)
    * [How is it different than other databases?](#how-is-it-different-than-other-databases)
* [Getting started](#getting-started)
* [A tour of the language](#a-tour-of-the-language)
  * [Variable Operations](#variable-operations)
  * [Array Operations](#array-operations)
    * [Insert](#insert)
    * [Write](#write)
    * [Read](#read)
    * [Remove](#remove)
  * [Transactions](#transactions)
  * [The Numstore Type System](#the-numstore-type-system)
  * [Data Inputs and Outputs](#data-inputs-and-outputs)

What is Numstore?
=================

Numstore is a database for arrays. It is an ACID database meant to store
contiguous arrays of numerical values. 

Features include:

1. Transaction Support 
2. Durability on power failure 
3. O(log N) inner array mutations (insert / remove)
4. A simple but extendable type system that mimics numpy's dtype

How is it different than other databases?
-----------------------------------------

SQL is great. It's versatile and in record-oriented computing, SQL solves the
data storage problem very well. Numstore doesn't compete with SQL. Numstore is
meant to store a different shape of data: _arrays_.

In traditional relational databases, data is organized into _Tables_ (a
singular _type_) which are further organized into _columns_. Columns are named
quantities. Each column has a name, and to access certain data you specify what
table(s) you want and what column(s) you want. This is tabular data.

Would you ever store a 256x256 pixel image in a relational database? Let's
think of ways to do this:

1. Store each pixel in its own column - up to pixel 65536. This breaks down
   easily with column limits. Postgres limits columns to 1600. For small
   arrays this is a tempting idea. A 5 element long array of floats could just
   be stored as 5 columns but in general, arrays are things where the "names"
   of columns are implicitly the index where they reside.

2. Storing a reference to the file system data. This is probably the most
   common approach to storing high-dimensionality data in databases. You store
   the location of the file on disk (e.g. 'file:///foo/bar/biz/image.png') or
   where it is on the internet
   (e.g. 'https://amazonaws.com/foo/bar/biz/image.png'). This is nice, but now
   you have two sources of truth. What happens if you move the image and don't
   update the database? Fundamentally, you're removing all the ACID properties
   of the database in favor of just storing the image in an unreliable file
   system.

3. Store the image data as some internal column type. We could store the image
   pixels as a BYTEA or something similar. This solves some of the problems of
   (2), but it doesn't always make sense to store large files in a database
   column. This often bloats database size and degrades memory consumption
   because fundamentally, relational databases weren't built for that type of
   data.

A fundamental limitation of relational databases is that _they don't support
packed high-dimensional data natively_. There are file formats out there:

1. HDF5
2. Parquet
3. Zarr-Python

But each one makes it very clear that it isn't a database because:

1. Databases should support concurrent writers / readers and logical isolation
   meaning two threads don't know about each other's work on the same data but
   keep the database in a consistent state.

2. Databases should support transactions - sure HDF5 is atomic, but you can't
   do multiple things to an HDF5 file atomically.

Numstore is the database for this type of data.

Getting started
===============

Everything below is a real Python session. Install the bindings:

```sh
pip install pynumstore
```

And open a database. A Numstore database is a single file, and there's no
server to start:

```python
>>> import numpy as np
>>> import pynumstore as ns
>>> db = ns.Database("tour.db")
>>> db
<Database 'tour.db'>
>>> db.path
'tour.db'
```

`Database` is a context manager, so in a script you'd normally write
`with ns.Database("tour.db") as db:` and let the block close it. In the REPL
it's easier to keep `db` around and call `db.close()` when you're done.

If you want to follow along, type the examples in order - each section builds on
the one before it.

A tour of the language
======================

To get a better glimpse of what Numstore can do, it helps to understand the
Numstore language, which is very self-contained and simple.

Numstore's query language is not SQL. This was a very intentional decision. A
Numstore database has seven fundamental operations:

1. `CREATE`
2. `GET`
3. `DELETE`
4. `INSERT`
5. `WRITE`
6. `READ`
7. `REMOVE`

You could put `CREATE`, `GET` and `DELETE` in their own category of _variable
operations_ and `INSERT`, `WRITE`, `READ` and `REMOVE` in their own category of
_array operations_.

From Python, a query is just a string. Which method you hand it to depends on
what the query needs to move across the boundary - nothing, metadata, data in,
or data out:

| Method               | Queries it accepts        | Returns                         |
|----------------------|---------------------------|---------------------------------|
| `db.execute(q)`      | `CREATE` `DELETE` `REMOVE`| `None`                          |
| `db.get(q)`          | Anything with a variable  | a `Var` (metadata)              |
| `db.write(q, data)`  | `INSERT` `WRITE`          | `int` - elements written        |
| `db.read(q)`         | `READ` `REMOVE`           | a fresh `numpy` array           |
| `db.read_into(q, o)` | `READ` `REMOVE`           | `int` - elements read into `o`  |

Hand a query to the wrong method and you get an error rather than a surprise:

```python
>>> db.execute("read foo")
Traceback (most recent call last):
  ...
RuntimeError: Only supported exec commands are REMOVE/CREATE/DELETE
```

A query that isn't a query at all fails the same way:

```python
>>> db.execute("not a real query")
Traceback (most recent call last):
  ...
RuntimeError: Expected a valid operation at pos: 0
```

Variable Operations
-------------------

`CREATE`, `GET` and `DELETE` exist because a Numstore database is really just a
map of "variables" to "arrays".

A Numstore database may look like this:
```
"foo"  ->  [1, 2, 3, 4, 5, ...]
"bar"  ->  [1, 3, 8, 9, 10, ...]
"biz"  ->  [3, 6, 7, 8, 9, ...]
...
```

To create new variables you execute the `CREATE` operation. `create` takes a
name and a type, and returns nothing - so in the REPL a successful `create` is
silent:

```python
>>> db.execute("create foo u32")
>>> db.execute("create bar f32")
>>> db.execute("create biz cf128")
```

You can get a variable's information by invoking the `GET` operation.
`db.get` hands you back a `Var`, a frozen snapshot of the variable's metadata:

```python
>>> db.get("get foo")
Var(name='foo', type='u32', length=0, tsize=4)
```

You can also run get for any query that contains a variable in it 
(the query never runs - so you only ever read the variable, not 
the data)
```python
>>> db.get("read foo[0:10]")
Var(name='foo', type='u32', length=0, tsize=4)
>>> db.get("remove foo[0:10]") # I don't modify foo
Var(name='foo', type='u32', length=0, tsize=4)
```

1. `name` is the variable name
2. `type` is the data type of the array - explained further in
   [The Numstore Type System](#the-numstore-type-system)
3. `length` is the number of elements in the array
4. `tsize` is the size in bytes of each element inside the array `foo` (a `u32`
   is 4 bytes)

A `Var` is a dataclass, so the fields are just attributes, and it carries two
numpy-shaped conveniences on top:

```python
>>> var = db.get("get foo")
>>> var.name, var.type
('foo', 'u32')
>>> var.length, var.tsize
(0, 4)
>>> var.dtype
dtype('uint32')
>>> var.shape
(0,)
>>> len(var)
0
```

`var.dtype` is the numpy dtype one element maps to, and `var.shape` is the shape
an array of the whole variable would have - here `(0,)`, because `foo` is still
empty. A `Var` is a snapshot, not a live view: it keeps reporting what was true
when you asked.

`CREATE` is idempotent as long as the type agrees. Re-creating a variable with
the same type leaves it (and its data) alone:

```python
>>> db.execute("create foo u32")
```

Asking for a different type for a name that already exists is an error, not a
silent replacement:

```python
>>> db.execute("create foo f64")
Traceback (most recent call last):
  ...
RuntimeError: Trying to create variable: foo but variable already exists and types are different
```

You can delete a variable by invoking the `DELETE` operation:

```python
>>> db.execute("delete biz")
>>> db.get("get biz")
Traceback (most recent call last):
  ...
RuntimeError: Variable: biz doesn't exist
```

Array Operations
----------------

Each variable has one array. So first, you need to create a variable:

```python
>>> db.execute("create data u32")
>>> db.get("get data")
Var(name='data', type='u32', length=0, tsize=4)
```

The only way to _add_ data to an array (e.g. increase the array size) is to
`INSERT` data into the array. `WRITE` doesn't increase the array size, it only
overwrites data. `INSERT` and `REMOVE` are the only length-modifying
operations.

### INSERT

To `INSERT` data, you specify what _offset_ to insert at and how many elements
you're inserting. The data itself goes to `db.write` as the second argument:

```python
>>> db.write("insert data 0 5", [1, 2, 3, 4, 5])
5
>>> db.read("read data")
array([1, 2, 3, 4, 5], dtype=uint32)
```

The general form of an insert command looks like this:

```
insert <Variable Name> <Offset> <Count>
```

* _Offset_ (`0` in the example above) is where to insert the data into the
  array. Negative numbers count back from the end of the array, -1 being the
  last index.
* _Count_ (`5` in the example above) is how many elements to insert. It's a
  ceiling, not a promise - see [Count is a ceiling](#count-is-a-ceiling).

The return value is the number of elements actually inserted.

For example, if we insert into a 6 element array at index 2, it's like we're
"pushing" data into index 2:

```
insert foo 2 3   <- [7, 8, 9]

Before:
  [1, 2, 3, 4, 5, 6]
         ^
     [7, 8, 9]

After:
  [1, 2, 7, 8, 9, 3, 4, 5, 6]
```

```
insert foo 0 3   <- [7, 8, 9]

Before:
  [1, 2, 3, 4, 5, 6]
   ^
[7, 8, 9]

After:
  [7, 8, 9, 1, 2, 3, 4, 5, 6]
```

```
insert foo -1 3  <- [7, 8, 9]

Before:
  [1, 2, 3, 4, 5, 6]
                  ^
              [7, 8, 9]

After:
  [1, 2, 3, 4, 5, 7, 8, 9, 6]
```

And here it is for real. `data` is `[1, 2, 3, 4, 5]` from above:

```python
>>> db.write("insert data 2 2", [7, 8])
2
>>> db.read("read data")
array([1, 2, 7, 8, 3, 4, 5], dtype=uint32)
>>> db.write("insert data -1 1", [9])
1
>>> db.read("read data")
array([1, 2, 7, 8, 3, 4, 9, 5], dtype=uint32)
```

A true `append` is an offset equal to the length of the array - or any offset
past it, because offsets clamp:

```python
>>> v = db.get("get data")
>>> db.write(f"insert data {len(v)} 2", [10, 11])
2
>>> db.read("read data")
array([ 1,  2,  7,  8,  3,  4,  9,  5, 10, 11], dtype=uint32)
```

#### Count is a ceiling

The _Count_ in the query and the length of the data you pass don't have to
agree. `INSERT` pairs them up and stops as soon as either one runs out, and the
return value tells you how many elements actually landed:

```python
>>> db.execute("create cap u32")
>>> db.write("insert cap 0 3", [1, 2])
2
>>> db.write("insert cap 0 2", [7, 8, 9])
2
>>> db.read("read cap")
array([7, 8, 1, 2], dtype=uint32)
```

The first call asked for 3 but only had 2 elements to give. The second had 3
elements but only asked for 2, so `9` was never consumed.

### WRITE

While `INSERT` increases the array length, `WRITE` simply overwrites data in
place. To write data, you specify the _range_ you wish to modify:

```python
>>> db.execute("create w u32")
>>> db.write("insert w 0 6", [1, 2, 3, 4, 5, 6])
6
>>> db.write("write w[2:5]", [70, 80, 90])
3
>>> db.read("read w")
array([ 1,  2, 70, 80, 90,  6], dtype=uint32)
```

The general form of a write command looks like this:

```
write <Variable Name>[ "[" ( index
      | [ start ] ":" [ stop ] [ ":" [ step ] ]
      ) "]" ]
```

Every part of the range is optional:

* _start_ is the first index written to. If it isn't included, it defaults to
  `0`. Negative numbers count back from the end of the array, -1 being the last
  index.
* _stop_ is the index to stop at, **exclusive**. If it isn't included, it
  defaults to the length of the array. Negative numbers count back from the
  end.
* _step_ is the distance between written indexes. If it isn't included, it
  defaults to `1`. A step of `0` is an error.

Assuming `w` is a 6 element array, the following commands all cover the whole
thing:

```python
>>> db.write("write w[0:6:1]", [1, 2, 3, 4, 5, 6])
6
>>> db.write("write w[0:]", [1, 2, 3, 4, 5, 6])
6
>>> db.write("write w[:]", [1, 2, 3, 4, 5, 6])
6
>>> db.write("write w", [1, 2, 3, 4, 5, 6])
6
```

Nothing moves, and the array is the same length afterwards:

```
write foo[2:5]  <- [7, 8, 9]
Before:
  [1, 2, 3, 4, 5, 6]
         ^  ^  ^
        [7, 8, 9]
After:
  [1, 2, 7, 8, 9, 6]
```

Compare that to `insert foo 2 3`, which pushes the old data out of the way and
leaves you with a 9 element array. If you want to grow an array, you want
`INSERT`.

A _step_ spreads the write out over the array instead of laying it down
contiguously:

```
write foo[0:6:2]  <- [7, 8, 9]
Before:
  [1, 2, 3, 4, 5, 6]
   ^     ^     ^
   7     8     9
After:
  [7, 2, 8, 4, 9, 6]
```

```python
>>> db.write("write w[0:6:2]", [7, 8, 9])
3
>>> db.read("read w")
array([7, 2, 8, 4, 9, 6], dtype=uint32)
```

A negative _step_ would walk the array backwards. Negative steps aren't
supported yet but are a planned feature:

```python
>>> db.write("write w[0:6:-2]", [7, 8, 9])
Traceback (most recent call last):
  ...
RuntimeError: stride step must be positive
```

A step of `0` is rejected by the same check:

```python
>>> db.write("write w[0:6:0]", [7, 8, 9])
Traceback (most recent call last):
  ...
RuntimeError: stride step must be positive
```

The range and the data don't have to agree on length. `WRITE` pairs them up one
element at a time and stops as soon as either one runs out, and returns how many
elements it wrote. If the data runs out first, the rest of the range is left
exactly as it was:

```python
>>> db.write("write w", [1, 2, 3, 4, 5, 6])
6
>>> db.write("write w[:]", [70, 80])
2
>>> db.read("read w")
array([70, 80,  3,  4,  5,  6], dtype=uint32)
```

And if the range runs out first, the remaining data is simply not consumed:

```python
>>> db.write("write w[0:2]", [11, 12, 13, 14, 15])
2
>>> db.read("read w")
array([11, 12,  3,  4,  5,  6], dtype=uint32)
```

An empty range is legal and writes nothing. In particular, writing to the range
just past the end of an array is **not** an `append`, it's a no-op that returns
`0`:

```python
>>> db.write("write w[6:]", [7, 8, 9])
0
>>> db.read("read w")
array([11, 12,  3,  4,  5,  6], dtype=uint32)
```

#### Ranges clamp

Ranges aren't bounds-checked the way an index into a Python list is. _start_ and
_stop_ are clamped to the array, so a range that hangs off either end just
covers less than you asked for:

```python
>>> db.write("write w[0:100]", [21, 22])
2
>>> db.write("write w[-100:]", [31, 32])
2
>>> db.read("read w")
array([31, 32,  3,  4,  5,  6], dtype=uint32)
```

#### A bare index

A single number in the brackets, with no colon, is a bare index: it covers
exactly one element. The colon is what makes a range open ended, so `w[2]` and
`w[2:]` are different things:

```python
>>> db.read("read w[2]")
array([3], dtype=uint32)
>>> db.read("read w[2:]")
array([3, 4, 5, 6], dtype=uint32)
```

`w[2]` and `w[2:3]` are the same thing. A bare index works wherever a range
does, so it's the way to touch a single element with any of the three
operations:

```python
>>> db.write("write w[2]", [70, 80, 90])
1
>>> db.read("read w")
array([31, 32, 70,  4,  5,  6], dtype=uint32)
```

The write asked to lay down three elements but the range only had room for one,
so only one landed - see the pairing rule above.

A bare index counts from the end like any other, and clamps like any other, so
an index past the end is an empty range rather than an error:

```python
>>> db.read("read w[-1]")
array([6], dtype=uint32)
>>> db.read_into("read w[6]", np.zeros(1, dtype=np.uint32))
0
```

### READ

To `READ` data, you specify the _range_ you wish to read:

```python
>>> db.execute("create r u32")
>>> db.write("insert r 0 6", [1, 2, 3, 4, 5, 6])
6
>>> db.read("read r[2:5]")
array([3, 4, 5], dtype=uint32)
```

The general form of a read command looks like this:

```
read <Variable Name>[ "[" ( index
     | [ start ] ":" [ stop ] [ ":" [ step ] ]
     ) "]" ]
```

`READ` has the same range semantics as `WRITE`. See [WRITE](#write).

Where `WRITE` puts data into the range, `READ` takes it out. `db.read` allocates
a fresh numpy array of exactly the right dtype and shape and hands it to you.

Assuming `r` is a 6 element array, the following commands do the same thing:

```python
>>> db.read("read r[0:6:1]")
array([1, 2, 3, 4, 5, 6], dtype=uint32)
>>> db.read("read r[0:]")
array([1, 2, 3, 4, 5, 6], dtype=uint32)
>>> db.read("read r[:]")
array([1, 2, 3, 4, 5, 6], dtype=uint32)
>>> db.read("read r")
array([1, 2, 3, 4, 5, 6], dtype=uint32)
```

Reading never modifies the array:

```
read foo[2:5]

Before:
  [1, 2, 3, 4, 5, 6]
         ^  ^  ^

Data:
  [3, 4, 5]

After:
  [1, 2, 3, 4, 5, 6]
```

A _step_ spreads the read out over the array, the same way it spreads out a
write:

```
read foo[0:6:2]

Before:
  [1, 2, 3, 4, 5, 6]
   ^     ^     ^

Data:
  [1, 3, 5]
```

```python
>>> db.read("read r[0:6:2]")
array([1, 3, 5], dtype=uint32)
```

Negative numbers count back from the end:

```python
>>> db.read("read r[-3:]")
array([4, 5, 6], dtype=uint32)
```

Negative steps aren't supported yet, so you can't read an array backwards:

```python
>>> db.read("read r[0:6:-2]")
Traceback (most recent call last):
  ...
RuntimeError: stride step must be positive
```

An empty range is legal, but `db.read` has nothing to allocate for it and
raises. If you want "read whatever is there, possibly nothing", use
`db.read_into`, which reports `0`:

```python
>>> db.read("read r[6:]")
Traceback (most recent call last):
  ...
RuntimeError: numstore operation failed
>>> db.read_into("read r[6:]", np.zeros(3, dtype=np.uint32))
0
```

Reading a variable that has no data at all is fine, though - you get an empty
array back:

```python
>>> db.execute("create empty u32")
>>> db.read("read empty")
array([], dtype=uint32)
```

### REMOVE

To `REMOVE` data, you specify the _range_ you wish to remove:

```python
>>> db.execute("create x u32")
>>> db.write("insert x 0 6", [1, 2, 3, 4, 5, 6])
6
>>> db.execute("remove x[2:5]")
>>> db.read("read x")
array([1, 2, 6], dtype=uint32)
```

The general form of a remove command looks like this:

```
remove <Variable Name>[ "[" ( index
       | [ start ] ":" [ stop ] [ ":" [ step ] ]
       ) "]" ]
```

`REMOVE` has the same range semantics as `READ`. See [READ](#read).

`REMOVE` is the other half of `INSERT`: it's the only operation besides `INSERT`
that changes the length of an array. Everything the range covers is taken out
and whatever is left closes the gap.

`REMOVE` is the one operation that answers to two methods. `db.execute` throws
the removed data away; `db.read` hands it back to you on the way out, which
makes it a destructive read:

```python
>>> db.write("insert x 0 3", [7, 8, 9])
3
>>> db.read("read x")
array([7, 8, 9, 1, 2, 6], dtype=uint32)
>>> db.read("remove x[0:2]")
array([7, 8], dtype=uint32)
>>> db.read("read x")
array([9, 1, 2, 6], dtype=uint32)
```

Either way the variable's length follows the data, so `get` after a remove
reports what's actually left:

```python
>>> db.get("get x")
Var(name='x', type='u32', length=4, tsize=4)
>>> db.execute("remove x[0]")
>>> db.get("get x")
Var(name='x', type='u32', length=3, tsize=4)
>>> db.read("read x")
array([1, 2, 6], dtype=uint32)
```

For example, removing the range `2:5` from a 6 element array takes out three
elements and leaves a 3 element array behind:

```
remove foo[2:5]

Before:
  [1, 2, 3, 4, 5, 6]
         ^  ^  ^

After:
  [1, 2, 6]
```

A _step_ punches holes in the array instead of taking out one contiguous block.
The array still closes up afterwards:

```
remove foo[0:6:2]

Before:
  [1, 2, 3, 4, 5, 6]
   ^     ^     ^

After:
  [2, 4, 6]
```

```python
>>> db.execute("remove x[0:3:2]")
>>> db.read("read x")
array([2], dtype=uint32)
```

Because the whole range is optional, the following commands all remove
everything. This empties the variable out, it doesn't get rid of the variable
itself - you're left with an array of length 0:

```
remove foo

Before:
  [1, 2, 3, 4, 5, 6]
   ^  ^  ^  ^  ^  ^

After:
  []
```

```python
>>> db.execute("remove x")
>>> db.read("read x")
array([], dtype=uint32)
```

An empty range is legal and removes nothing:

```python
>>> db.write("insert x 0 3", [1, 2, 3])
3
>>> db.execute("remove x[6:]")
>>> db.read("read x")
array([1, 2, 3], dtype=uint32)
```

Transactions
------------

Every one of the operations above ran on its own. When you don't name a
transaction, each query gets an automatic one: it either happens completely or
not at all, and it's durable by the time the call returns.

To group several operations into one atomic unit, open a transaction with
`db.begin()` and run the queries on it instead of on `db`. A `Transaction` has
the same five query methods as a `Database`:

```python
>>> db.execute("create prices f64")
>>> txn = db.begin()
>>> txn
<Transaction open>
>>> txn.write("insert prices 0 3", [1.5, 2.25, 3.75])
3
>>> txn.read("read prices")
array([1.5 , 2.25, 3.75])
>>> txn.commit()
>>> db.read("read prices")
array([1.5 , 2.25, 3.75])
```

Inside the transaction, `txn.read` sees the transaction's own uncommitted work.
`rollback` throws all of it away:

```python
>>> txn = db.begin()
>>> txn.write("insert prices 3 2", [4.0, 5.0])
2
>>> txn.read("read prices")
array([1.5 , 2.25, 3.75, 4.  , 5.  ])
>>> txn.rollback()
>>> db.read("read prices")
array([1.5 , 2.25, 3.75])
```

A transaction is finished once you commit or roll it back, and using it again is
an error rather than a silent no-op:

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
...     txn.write("insert prices 3 2", [4.0, 5.0])
2
>>> db.read("read prices")
array([1.5 , 2.25, 3.75, 4.  , 5.  ])
```

That `2` is the REPL echoing the return value of `txn.write` from inside the
block - the two elements it inserted.

If the block raises, nothing lands:

```python
>>> try:
...     with db.begin() as txn:
...         txn.write("insert prices 0 2", [98.0, 99.0])
...         raise ValueError("something went wrong")
... except ValueError as exc:
...     print(exc)
2
something went wrong
>>> db.read("read prices")
array([1.5 , 2.25, 3.75, 4.  , 5.  ])
```

The Numstore Type System
------------------------

In all the previous examples, we created _arrays of scalars_. Numstore supports
far more than arrays of scalars, in fact, our original example was an array of
`256x256` images! There are 4 main types in Numstore:
1. `Primitive`
2. `Struct`
3. `Union`
4. `Strict Array`

Everything in Numstore is built out of these 4. Structs, unions, and arrays are
all ways of gluing other types together, and at the bottom of every type is a
`Primitive`. Once you know how to size these 4, you can size any type in
Numstore.

Every Numstore type maps onto a numpy dtype, and `ns.to_dtype` shows you the
mapping without touching a database:

```python
>>> ns.to_dtype("u32")
dtype('uint32')
>>> ns.to_dtype("u32").itemsize
4
```

#### Primitives

`Primitives` are the core data type of Numstore. They are categorized into
signed/unsigned, floats/integers, complex/real, and number of bits.

The signed ints are:
* `i8` - a 1 byte signed integer
* `i16` - a 2 byte signed integer
* `i32` - a 4 byte signed integer
* `i64` - an 8 byte signed integer

Integers can be unsigned too:
* `u8` - a 1 byte unsigned integer
* `u16` - a 2 byte unsigned integer
* `u32` - a 4 byte unsigned integer
* `u64` - an 8 byte unsigned integer

Numstore also supports floating point numbers:
* `f16` - a 2 byte float
* `f32` - a 4 byte float
* `f64` - an 8 byte float
* `f128` - a 16 byte float

Finally, any of the above types can also be complex (effectively doubling their
size):
* `ci16-128` - Complex signed ints
* `cu16-128` - Complex unsigned ints
* `cf32-256` - Complex floats

The number in a primitive's name is always its size **in bits**, so the byte
size is just `bits / 8`. A complex type stores a real part and an imaginary part
back to back, which is why its name is double the bits of the real type it's
built from: `cf64` is two `f32`s, so `64 / 8 = 8` bytes.

Here's the full key:

| Type    | Bits | Bytes | Notes                         |
|---------|------|-------|-------------------------------|
| `i8`    | 8    | 1     | signed integer                |
| `i16`   | 16   | 2     | signed integer                |
| `i32`   | 32   | 4     | signed integer                |
| `i64`   | 64   | 8     | signed integer                |
| `u8`    | 8    | 1     | unsigned integer              |
| `u16`   | 16   | 2     | unsigned integer              |
| `u32`   | 32   | 4     | unsigned integer              |
| `u64`   | 64   | 8     | unsigned integer              |
| `f16`   | 16   | 2     | float                         |
| `f32`   | 32   | 4     | float                         |
| `f64`   | 64   | 8     | float                         |
| `f128`  | 128  | 16    | float                         |
| `ci16`  | 16   | 2     | complex `i8`  (2 x `i8`)      |
| `ci32`  | 32   | 4     | complex `i16` (2 x `i16`)     |
| `ci64`  | 64   | 8     | complex `i32` (2 x `i32`)     |
| `ci128` | 128  | 16    | complex `i64` (2 x `i64`)     |
| `cu16`  | 16   | 2     | complex `u8`  (2 x `u8`)      |
| `cu32`  | 32   | 4     | complex `u16` (2 x `u16`)     |
| `cu64`  | 64   | 8     | complex `u32` (2 x `u32`)     |
| `cu128` | 128  | 16    | complex `u64` (2 x `u64`)     |
| `cf32`  | 32   | 4     | complex `f16` (2 x `f16`)     |
| `cf64`  | 64   | 8     | complex `f32` (2 x `f32`)     |
| `cf128` | 128  | 16    | complex `f64` (2 x `f64`)     |
| `cf256` | 256  | 32    | complex `f128` (2 x `f128`)   |

Numpy has a native dtype for most of these. Where it doesn't - the small complex
integers, and `cf32` - Numstore maps the type to a two-field structured dtype
with a real and an imaginary part, which has the same layout in memory:

```python
>>> ns.to_dtype("cf64")
dtype('complex64')
>>> ns.to_dtype("cu16")
dtype([('re', 'u1'), ('im', 'u1')])
```

`f128` and `cf256` need a 16 byte float, which numpy only has where the
platform's `long double` is 16 bytes wide. Numstore stores them anywhere - the
database doesn't care what numpy can represent:

```python
>>> db.execute("create wide f128")
>>> db.get("get wide").tsize
16
```

Reading or writing one, though, has to go through a dtype, so on a platform
without a wide long double (macOS on Apple silicon, for instance)
`ns.to_dtype("f128")` raises `NotImplementedError` and so does any `read` or
`write` of that variable. `get` keeps working, because it only reports sizes.

To create a primitive type, you specify the name right after the variable name
in your `create` query:

```python
>>> db.execute("create p1 u32")
>>> db.execute("create p2 ci128")
>>> db.execute("create p3 cu64")
```

The size of each element is the size of the primitive:

```python
>>> db.get("get p1")
Var(name='p1', type='u32', length=0, tsize=4)
>>> db.get("get p2")
Var(name='p2', type='ci128', length=0, tsize=16)
>>> db.get("get p3")
Var(name='p3', type='cu64', length=0, tsize=8)
```

`tsize` is the size of **one element** of the variable. A fresh variable has
`length` 0 - you haven't pushed any data in yet - but its `tsize` is locked in
the moment you create it, because that's fixed by the type.

#### Structs

A `Struct` is a product type. That essentially means sub types are "stacked" on
top of each other. Unlike programming languages, there is no struct padding.
Numstore structs are WYSIWYG.

In the following example:

```python
>>> db.execute("create s struct { a u32, b cf128 }")
```

`s` is a variable whose elements as bytes in memory look like:

`[ 4 byte int ][    16 byte complex float    ]`

Because there's no padding, **the size of a struct is just the sum of its
members**. Here that's `4 + 16 = 20` bytes:

```python
>>> db.get("get s")
Var(name='s', type='struct { a u32, b cf128 }', length=0, tsize=20)
```

Members are laid out in the order you write them, and each member starts exactly
where the previous one ended. `a` sits at byte offset 0, `b` sits at byte
offset 4. Numpy's dtype for the same type says so too:

```python
>>> ns.to_dtype("struct { a u32, b cf128 }")
dtype([('a', '<u4'), ('b', '<c16')])
```

A struct variable reads and writes as a numpy structured array, so you can hand
it a list of tuples and pull a single field back out by name:

```python
>>> db.execute("create pt struct { x f32, y f32, z f32 }")
>>> db.write("insert pt 0 2", [(1, 2, 3), (4, 5, 6)])
2
>>> db.read("read pt")
array([(1., 2., 3.), (4., 5., 6.)],
      dtype=[('x', '<f4'), ('y', '<f4'), ('z', '<f4')])
>>> db.read("read pt")["y"]
array([2., 5.], dtype=float32)
```

#### Unions

A `Union` is a sum type. That essentially means sub types are "overlayed" on top
of each other. Unlike programming languages, there is no union padding. Numstore
unions are WYSIWYG.

In the following example:

```python
>>> db.execute("create u union { a u32, b cf128 }")
```

`u` is a variable whose elements as bytes in memory look like:

```
a -> [ 4 byte int ]
b -> [      16 byte complex float      ]
     ^-- both members start at the same byte --^
```

Every member shares the same starting byte, so **the size of a union is the
size of its largest member** - here `max(4, 16) = 16` bytes:

```python
>>> db.get("get u")
Var(name='u', type='union { a u32, b cf128 }', length=0, tsize=16)
```

Reading `a` interprets the first 4 bytes as a `u32`; reading `b` interprets all
16 bytes as a `cf128`. It's the same memory - you just choose how to read it.
Numpy spells that as a structured dtype whose fields both sit at offset 0:

```python
>>> ns.to_dtype("union { a u32, b cf128 }")
dtype({'names': ['a', 'b'], 'formats': ['<u4', '<c16'], 'offsets': [0, 0], 'itemsize': 16})
```

#### Strict Arrays

A `Strict Array` is an array type. This is how we'd store our 256x256 image in
our database.

In the following example:

```python
>>> db.execute("create img [256][256] f32")
```

`img` is a 256x256 grid of `f32`s - exactly the image from our very first
example. You read the dimensions left to right, outermost first: `[256][256]
f32` means "a 256-element array, where each element is a 256-element array of
`f32`."

**The size of an array is the product of its dimensions times the size of its
element.** Each `f32` is 4 bytes and there are `256 x 256 = 65,536` of them, so
one `img` element is `65,536 x 4 = 262,144` bytes:

```python
>>> db.get("get img")
Var(name='img', type='[256][256]f32', length=0, tsize=262144)
```

A strict array variable is an array *of* arrays, so in numpy it picks up extra
trailing dimensions. The leading axis is the variable's length; the trailing
axes are the element's shape:

```python
>>> db.execute("create grid [2][3] u8")
>>> db.write("insert grid 0 2", np.arange(12, dtype=np.uint8).reshape(2, 2, 3))
2
>>> db.read("read grid").shape
(2, 2, 3)
>>> db.read("read grid[0:1]")
array([[[0, 1, 2],
        [3, 4, 5]]], dtype=uint8)
```

Ranges still index *elements* of the variable, not scalars - `grid[0:1]` is the
first 2x3 element, not the first byte.

##### Array ordering (C order)

Numstore stores arrays in **C order** (row-major), which means **the last
dimension is contiguous in memory** and the first dimension has the largest
stride. The last index varies fastest as you walk through memory.

For a small `[2][3] u8`, memory looks like:
```
[ (0,0) ][ (0,1) ][ (0,2) ][ (1,0) ][ (1,1) ][ (1,2) ]
```
Notice the second index (the column) races from 0->2 before the first index
(the row) ticks over. That's row-major. You can see it in the bytes:

```python
>>> db.read("read grid[0:1]").tobytes()
b'\x00\x01\x02\x03\x04\x05'
```

To find where element `[i][j]` lives in a `[R][C]` array of element size `S`,
the byte offset is:

`offset = (i * C + j) * S`

So in our `[256][256] f32`, element `[2][3]` sits at `(2 * 256 + 3) * 4 = 2060`
bytes into the element. This generalizes to any number of dimensions: multiply
each index by the product of all the dimensions to its right, sum them up, and
multiply by the element size.

#### Combining Types

A struct member can be an array, an array element can be a struct, a union can
hold an array, and so on - as deep as you like. The sizing rules compose, so you
never need a new rule, you just apply the same 4 recursively:

| Type          | Size                                             |
|---------------|--------------------------------------------------|
| `Primitive`   | `bits / 8`                                       |
| `Struct`      | sum of member sizes                              |
| `Union`       | max of member sizes                              |
| `Strict Array`| product of dimensions x element size             |

An **array of structs** - say a buffer of 3D points. The struct is
`4 + 4 + 4 = 12` bytes, and there are 1000 of them:

```python
>>> db.execute("create points [1000] struct { x f32, y f32, z f32 }")
>>> db.get("get points").tsize
12000
```

A **struct with an array member** - an image with a header. `timestamp` is 8
bytes, `pixels` is `256 x 256 x 1 = 65,536` bytes:

```python
>>> db.execute("create frame struct { timestamp u64, pixels [256][256] u8 }")
>>> db.get("get frame").tsize
65544
```

And our original **array of images**, now fully spelled out - 12 frames of
256x256 RGB. The struct is `1 + 1 + 1 = 3` bytes and the array is
`12 x 256 x 256 = 786,432` elements:

```python
>>> db.execute("create video [12][256][256] struct { r u8, g u8, b u8 }")
>>> db.get("get video").tsize
2359296
```

Same 4 rules, just stacked.

Bad types are caught when the query is compiled, before anything touches the
database:

```python
>>> db.execute("create bad q99")
Traceback (most recent call last):
  ...
RuntimeError: Expected type at position 2, got token type TT_IDENTIFIER
>>> ns.to_dtype("struct { i u8, i u32 }")
Traceback (most recent call last):
  ...
ValueError: duplicate key: i
```

#### Practice Problems

`ns.to_dtype(...).itemsize` will check your answers.

1. What is the element size of `struct { a u16, b i64, c f32 }`?
2. What is the element size of `union { a u16, b i64, c f32 }`?
3. What is the element size of `[10][20] u32`?
4. What is the element size of `[100] struct { x f32, y f32, z f32 }`?
5. What is the element size of `struct { id u64, samples [8] cf64 }`?
6. What is the element size of `[12][256][256] struct { r u8, g u8, b u8 }`?
7. In a `[4][4] f32`, what byte offset is element `[2][3]`?
8. What is the element size of
   `struct { tag u8, payload union { a f64, b [2] u32 } }`?

##### Answers

1. `2 + 8 + 4 = 14` bytes.
2. `max(2, 8, 4) = 8` bytes.
3. `10 x 20 x 4 = 800` bytes.
4. struct is `12`, so `100 x 12 = 1,200` bytes.
5. `cf64` is 8 bytes -> `samples` is `8 x 8 = 64`, plus `id` at 8 -> `72` bytes.
6. struct is `3`, array is `12 x 256 x 256 = 786,432` -> `2,359,296` bytes.
7. `(2 x 4 + 3) x 4 = 11 x 4 = 44` bytes.
8. `union` is `max(8, 2 x 4) = 8`, plus `tag` at 1 -> `9` bytes.

```python
>>> ns.to_dtype("struct { a u16, b i64, c f32 }").itemsize
14
>>> ns.to_dtype("union { a u16, b i64, c f32 }").itemsize
8
>>> ns.to_dtype("[10][20] u32").itemsize
800
>>> ns.to_dtype("[100] struct { x f32, y f32, z f32 }").itemsize
1200
>>> ns.to_dtype("struct { id u64, samples [8] cf64 }").itemsize
72
>>> ns.to_dtype("[12][256][256] struct { r u8, g u8, b u8 }").itemsize
2359296
>>> ns.to_dtype("struct { tag u8, payload union { a f64, b [2] u32 } }").itemsize
9
```

Data Inputs and Outputs
-----------------------

The query language says *which* elements to touch. The Python side decides where
the bytes come from and go.

### Getting data out

`db.read` allocates. It asks the variable for its dtype and shape, allocates an
array of exactly that, and hands it to you:

```python
>>> db.execute("create out u32")
>>> db.write("insert out 0 4", [1, 2, 3, 4])
4
>>> db.read("read out")
array([1, 2, 3, 4], dtype=uint32)
```

`db.read_into` doesn't allocate. You bring the buffer, it fills as much of it as
it can and returns how many elements it wrote. That makes it the tool for
reading a large variable in chunks, or for reading into memory you already own:

```python
>>> buf = np.zeros(2, dtype=np.uint32)
>>> db.read_into("read out", buf)
2
>>> buf
array([1, 2], dtype=uint32)
```

The buffer has to actually fit the variable's type. `read_into` checks the dtype
exactly - it won't convert on your behalf, because the whole point is to write
straight into your memory:

```python
>>> db.read_into("read out", np.zeros(4, dtype=np.float64))
Traceback (most recent call last):
  ...
TypeError: out has dtype float64, expected uint32
>>> db.read_into("read out", [0, 0, 0, 0])
Traceback (most recent call last):
  ...
TypeError: out must be a numpy array, not list
```

It must also be C-contiguous and writable, which rules out a non-trivial slice
of someone else's array.

### Putting data in

`db.write` accepts anything numpy can turn into an array. Lists, tuples, and
numpy arrays all work:

```python
>>> db.execute("create inp f64")
>>> db.write("insert inp 0 2", [1.5, 2.5])
2
>>> db.write("insert inp 2 2", (3.5, 4.5))
2
>>> db.write("insert inp 4 2", np.array([5.5, 6.5]))
2
>>> db.read("read inp")
array([1.5, 2.5, 3.5, 4.5, 5.5, 6.5])
```

When you pass a numpy array, its dtype is checked and only a same-kind cast is
allowed - so a widening cast goes through and a lossy one is refused:

```python
>>> db.write("write inp", np.array([1.5, 2.5], dtype=np.float32))
2
>>> db.execute("create ints u32")
>>> db.write("insert ints 0 2", np.array([1.5, 2.5]))
Traceback (most recent call last):
  ...
TypeError: cannot write float64 data to a uint32 variable
```

A plain list doesn't get that protection - it goes through
`np.asarray(data, dtype)`, which casts however numpy sees fit. If you care about
not silently truncating, pass an array:

```python
>>> db.write("insert ints 0 2", [1.5, 2.5])
2
>>> db.read("read ints")
array([1, 2], dtype=uint32)
```

### Shapes

For a scalar variable, the data is one-dimensional and only the element count
matters. For a strict array variable, the trailing dimensions have to match the
element type exactly, and the leading axis is the element count:

```python
>>> db.execute("create rows [3] u32")
>>> db.write("insert rows 0 2", np.arange(6, dtype=np.uint32).reshape(2, 3))
2
>>> db.write("insert rows 0 1", [[1, 2]])
Traceback (most recent call last):
  ...
ValueError: data has shape (1, 2), expected (n, 3)
>>> db.read_into("read rows", np.zeros((2, 2), dtype=np.uint32))
Traceback (most recent call last):
  ...
ValueError: out has shape (2, 2), expected (n, 3)
```

### Reusing a query

Every call above compiled its query string, ran it, and threw the compiled form
away. `db.prepare` keeps it, so a query you run in a loop is compiled once:

```python
>>> plan = db.prepare("read out[0:2]")
>>> plan
<Plan 'read out[0:2]'>
>>> plan.read()
array([1, 2], dtype=uint32)
>>> plan.read()
array([1, 2], dtype=uint32)
>>> plan.close()
```

A `Plan` has the same `execute` / `read` / `read_into` / `write` / `var` methods,
each taking an optional transaction as its last argument, and it's a context
manager like everything else:

```python
>>> with db.prepare("insert out 0 2") as plan:
...     with db.begin() as txn:
...         plan.write([10, 20], txn)
2
>>> db.read("read out")
array([10, 20,  1,  2,  3,  4], dtype=uint32)
```

When you're done with the database, close it. Any query after that is an error
rather than a crash:

```python
>>> db.close()
>>> db.closed
True
>>> db.read("read out")
Traceback (most recent call last):
  ...
RuntimeError: database is closed
```
