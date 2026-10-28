"""Seeded simulation tests against a reference model.

Six simulations, each aimed at one interaction surface:

  simulate_data         insert / write / remove / read on a single variable,
                        with the variable's whole contents re-verified after
                        every mutation
  simulate_page_boundaries
                        the same operations aimed deliberately at whole-page
                        boundaries, including blocks that span several pages
  simulate_schema       create / get / delete across many variables at once,
                        so one variable's schema operations cannot disturb
                        another's data
  simulate_reclaim      big variables churned through create/fill/delete,
                        against witnesses that must never change, so a page
                        handed back and reused shows up as damage elsewhere
  simulate_interleaved  several variables of different widths all under data
                        load at once, every one verified every step
  simulate_txn          the same data operations wrapped in transactions,
                        plus close/reopen

Runs two ways:

  * under pytest, as short runs that stay in the fast suite
  * as its own program, for long soak runs:

        python3 simulation.py --runs 200 --steps 400
        python3 simulation.py --seed 3934281107   # replay one failure

Every failure reports the seed, the step number and the action journal, so a
run that breaks is a regression test waiting to be copied out of the log.

Note: nothing here touches the database outside an open transaction. That
deadlocks by design, so the transaction simulation routes every operation in
a batch through the transaction handle.
"""

from __future__ import annotations

import argparse
import os
import random
import sys

import numpy as np
import pytest

import pynumstore as ns

# (numstore type, numpy dtype, element dims). Element dims shift the model from
# a list of scalars to a list of rows, which is where the shape bookkeeping in
# GET and in a strided read gets interesting.
TYPE_POOL = [
    ("u8", np.uint8, ()),
    ("u16", np.uint16, ()),
    ("i32", np.int32, ()),
    ("i64", np.int64, ()),
    ("f32", np.float32, ()),
    ("f64", np.float64, ()),
    ("[3]u32", np.uint32, (3,)),
    ("[2][2]u8", np.uint8, (2, 2)),
]


class Mismatch(AssertionError):
    """A divergence between the database and the reference model."""


############# The reference model


class Variable:
    """An obviously-correct numstore variable: a Python list of rows."""

    def __init__(self, name, type_str, dtype, dims):
        self.name = name
        self.type_str = type_str
        self.dtype = np.dtype(dtype)
        self.dims = dims
        self.rows: list = []

    def copy(self) -> Variable:
        clone = Variable(self.name, self.type_str, self.dtype, self.dims)
        clone.rows = list(self.rows)
        return clone

    ############# mutations, each returning what the query should report
    def insert(self, offset, values):
        self.rows[offset:offset] = values
        return len(values)

    def write(self, start, stop, values):
        n = min(stop - start, len(values))
        self.rows[start : start + n] = values[:n]
        return n

    def write_strided(self, start, stop, step, values):
        positions = range(start, stop, step)
        n = min(len(positions), len(values))
        for value, pos in zip(values[:n], positions[:n]):
            self.rows[pos] = value
        return n

    def remove(self, start, stop, step):
        dropped = set(range(start, stop, step))
        self.rows = [r for i, r in enumerate(self.rows) if i not in dropped]
        return len(dropped)

    ############# views
    def as_array(self, rows=None):
        rows = self.rows if rows is None else rows
        if not rows:
            return np.empty((0, *self.dims), dtype=self.dtype)
        return np.array(rows, dtype=self.dtype).reshape(-1, *self.dims)

    @property
    def shape(self):
        return (len(self.rows), *self.dims)


class World:
    """Every variable the database is supposed to contain."""

    def __init__(self):
        self.vars: dict[str, Variable] = {}

    def copy(self) -> World:
        clone = World()
        clone.vars = {name: var.copy() for name, var in self.vars.items()}
        return clone

    def names(self):
        return set(self.vars)


############# The harness


class Harness:
    """Drives one database against a World, verifying as it goes."""

    def __init__(self, rng, path, world=None):
        self.rng = rng
        self.path = path
        self.world = World() if world is None else world
        self.journal: list[str] = []
        self.step = 0
        self.db = ns.Database(path)

    def close(self):
        self.db.close()

    def log(self, text):
        self.journal.append(f"{self.step:>4}  {text}")

    def target(self, txn):
        return self.db if txn is None else txn

    ############# random data
    def values(self, var, n):
        """n random rows, returned already cast so the model holds exactly
        what was written -- float comparison stays exact that way."""
        width = int(np.prod(var.dims)) if var.dims else 1
        flat = [[self.scalar(var.dtype) for _ in range(width)] for _ in range(n)]
        return list(np.array(flat, dtype=var.dtype).reshape(n, *var.dims))

    def scalar(self, dtype):
        """Edge-biased: limits and small values far more often than midrange."""
        if dtype.kind == "f":
            info = np.finfo(dtype)
            return self.rng.choice(
                [
                    0.0,
                    -0.0,
                    1.0,
                    -1.0,
                    float(info.max),
                    float(info.min),
                    float(info.tiny),
                    np.inf,
                    -np.inf,
                    np.nan,
                    self.rng.uniform(-1e3, 1e3),
                ]
            )
        info = np.iinfo(dtype)
        return self.rng.choice(
            [
                info.min,
                info.min + 1,
                0,
                1,
                info.max - 1,
                info.max,
                self.rng.randint(int(info.min), int(info.max)),
            ]
        )

    ############# random ranges
    #
    # Every range generated here is non-empty. Zero-length operations are
    # rejected by the query layer rather than treated as no-ops (see the xfail
    # group in query.py), so generating one would be testing the wrong thing.
    def nonempty_range(self, var):
        start = self.rng.randint(0, len(var.rows) - 1)
        stop = self.rng.randint(start + 1, len(var.rows))
        return start, stop

    ############# verification
    def check(self, cond, what):
        if not cond:
            raise Mismatch(what)

    def verify_var(self, name, txn=None):
        """A variable's full contents and metadata, read back three ways."""
        var = self.world.vars[name]
        target = self.target(txn)
        expected = var.as_array()

        got = target.get(f"get {name}")
        self.check(
            got.length == len(var.rows),
            f"{name}: GET reports length {got.length}, model says {len(var.rows)}",
        )
        self.check(
            got.type == var.type_str,
            f"{name}: GET reports type {got.type!r}, model says {var.type_str!r}",
        )
        self.check(
            got.shape == var.shape,
            f"{name}: GET reports shape {got.shape}, model says {var.shape}",
        )
        self.check(
            got.dtype == var.dtype,
            f"{name}: GET reports dtype {got.dtype}, model says {var.dtype}",
        )

        # the allocating read
        np.testing.assert_array_equal(
            target.read(f"read {name}"), expected, err_msg=f"{name}: read mismatch"
        )

        # and the caller-buffer read, into a buffer with room to spare: it must
        # report the real count and leave everything past that alone
        pad = 3
        out = np.zeros((len(var.rows) + pad, *var.dims), dtype=var.dtype)
        sentinel = np.array(7, dtype=var.dtype)
        out[len(var.rows) :] = sentinel
        n = target.read_into(f"read {name}", out)
        self.check(
            n == len(var.rows),
            f"{name}: read_into read {n}, model says {len(var.rows)}",
        )
        np.testing.assert_array_equal(
            out[:n], expected, err_msg=f"{name}: read_into mismatch"
        )
        self.check(
            bool((out[len(var.rows) :] == sentinel).all()),
            f"{name}: read_into wrote past the {n} rows it reported",
        )

    def verify_world(self, txn=None):
        """Every variable, plus the variable list itself."""
        target = self.target(txn)

        listed = {v.name for v in target.get_all()}
        self.check(
            listed == self.world.names(),
            f"get_all reports {sorted(listed)}, model says {sorted(self.world.names())}",
        )

        for name in sorted(self.world.names()):
            self.verify_var(name, txn)

    def reopen(self):
        self.log("close + reopen")
        self.db.close()
        self.db = ns.Database(self.path)


############# Simulation 1: insert / write / remove / read on one variable
#
# The point of this one is that every mutation is followed by a full read-back
# of the variable, so a corruption is reported at the step that caused it
# rather than hundreds of steps later.


def insert_at(h, var, offset, n, txn=None):
    """INSERT n rows at an offset the caller chose."""
    vals = h.values(var, n)

    actual = h.target(txn).write(f"insert {var.name} {offset} {n}", var.as_array(vals))
    h.log(f"insert {var.name} {offset} {n} -> {actual}")

    expected = var.insert(offset, vals)
    h.check(actual == expected, f"insert returned {actual}, model says {expected}")


def remove_at(h, var, start, stop, step=1, txn=None):
    """REMOVE a range the caller chose."""
    h.target(txn).execute(f"remove {var.name}[{start}:{stop}:{step}]")
    h.log(f"remove {var.name}[{start}:{stop}:{step}]")

    var.remove(start, stop, step)


def data_insert(h, var, txn=None):
    insert_at(h, var, h.rng.randint(0, len(var.rows)), h.rng.randint(1, 8), txn)


def data_write(h, var, txn=None):
    if not var.rows:
        return data_insert(h, var, txn)

    start, stop = h.nonempty_range(var)
    # sometimes fewer rows than the range, so the write is bounded by the data
    vals = h.values(var, h.rng.randint(1, stop - start))

    actual = h.target(txn).write(
        f"write {var.name}[{start}:{stop}]", var.as_array(vals)
    )
    h.log(f"write {var.name}[{start}:{stop}] ({len(vals)} rows) -> {actual}")

    expected = var.write(start, stop, vals)
    h.check(actual == expected, f"write returned {actual}, model says {expected}")


def data_write_strided(h, var, txn=None):
    if not var.rows:
        return data_insert(h, var, txn)

    start, stop = h.nonempty_range(var)
    step = h.rng.choice([1, 2, 3])
    positions = len(range(start, stop, step))
    vals = h.values(var, h.rng.randint(1, max(positions, 1)))

    actual = h.target(txn).write(
        f"write {var.name}[{start}:{stop}:{step}]", var.as_array(vals)
    )
    h.log(f"write {var.name}[{start}:{stop}:{step}] ({len(vals)} rows) -> {actual}")

    expected = var.write_strided(start, stop, step, vals)
    h.check(
        actual == expected, f"strided write returned {actual}, model says {expected}"
    )


def data_remove(h, var, txn=None):
    if not var.rows:
        return data_insert(h, var, txn)

    start, stop = h.nonempty_range(var)
    remove_at(h, var, start, stop, h.rng.choice([1, 1, 1, 2, 3]), txn)


def data_destructive_read(h, var, txn=None):
    """A read of a REMOVE hands back the rows it drops."""
    if not var.rows:
        return data_insert(h, var, txn)

    start, stop = h.nonempty_range(var)
    expected = var.as_array(var.rows[start:stop])

    got = h.target(txn).read(f"remove {var.name}[{start}:{stop}]")
    h.log(f"read(remove {var.name}[{start}:{stop}])")

    np.testing.assert_array_equal(
        got, expected, err_msg="destructive read returned the wrong rows"
    )
    var.remove(start, stop, 1)


############# Page geometry
#
# NS_PAGE_SIZE is 4096 (csrc/core/ns_stdtypes.h). A data-list page spends a
# header on its links and length, so the real payload is a little under that.
# The exact offset deliberately does not matter here: the boundary simulation
# churns a window wide enough to straddle whatever the true boundary is.
PAGE_BYTES = 4096


def rows_per_page(var):
    """Roughly how many rows of this variable fit in one page."""
    width = int(np.prod(var.dims)) if var.dims else 1
    return max(PAGE_BYTES // (var.dtype.itemsize * width), 1)


def data_read_slice(h, var, txn=None):
    if not var.rows:
        return

    start, stop = h.nonempty_range(var)
    step = h.rng.choice([1, 1, 2, 3])

    got = h.target(txn).read(f"read {var.name}[{start}:{stop}:{step}]")
    h.log(f"read {var.name}[{start}:{stop}:{step}]")

    np.testing.assert_array_equal(
        got, var.as_array(var.rows[start:stop:step]), err_msg="sliced read mismatch"
    )


def data_read_bare_index(h, var, txn=None):
    """`v[i]` is exactly one element, not an open range."""
    if not var.rows:
        return

    i = h.rng.randint(0, len(var.rows) - 1)

    got = h.target(txn).read(f"read {var.name}[{i}]")
    h.log(f"read {var.name}[{i}] (bare)")

    np.testing.assert_array_equal(
        got, var.as_array(var.rows[i : i + 1]), err_msg="bare index read mismatch"
    )


def data_read_negative(h, var, txn=None):
    if not var.rows:
        return

    k = h.rng.randint(1, len(var.rows))

    got = h.target(txn).read(f"read {var.name}[-{k}:]")
    h.log(f"read {var.name}[-{k}:]")

    np.testing.assert_array_equal(
        got, var.as_array(var.rows[-k:]), err_msg="negative range read mismatch"
    )


DATA_MUTATIONS = [
    (data_insert, 6),
    (data_write, 3),
    (data_write_strided, 2),
    (data_remove, 3),
    (data_destructive_read, 2),
]
DATA_READS = [
    (data_read_slice, 3),
    (data_read_bare_index, 2),
    (data_read_negative, 2),
]


def simulate_data(seed, steps, type_str, dtype, dims, path):
    """One variable, every data operation, verified after every mutation."""
    rng = random.Random(seed)
    h = Harness(rng, path)

    var = Variable("v", type_str, dtype, dims)
    h.world.vars["v"] = var
    h.db.execute(f"create v {type_str}")

    mutations = [a for a, w in DATA_MUTATIONS for _ in range(w)]
    reads = [a for a, w in DATA_READS for _ in range(w)]

    try:
        h.verify_var("v")
        for step in range(steps):
            h.step = step

            # a mutation, then the whole variable read back
            rng.choice(mutations)(h, var)
            h.verify_var("v")

            # then a couple of reads that must agree with the same model
            for _ in range(rng.randint(0, 2)):
                rng.choice(reads)(h, var)

            # and occasionally prove it all survives a close/reopen
            if rng.random() < 0.02:
                h.reopen()
                h.verify_var("v")

        h.reopen()
        h.verify_world()
    except Exception as exc:
        raise _report("data", seed, type_str, h, exc) from exc
    finally:
        h.close()


############# Simulation 2: operations that straddle a page boundary
#
# Every other simulation inserts a handful of rows at a time, so it only ever
# crosses a page boundary incidentally, never inside one operation. This one
# aims at the boundary on purpose:
#
#   1. a block insert lands the length just short of k whole pages
#   2. small inserts and removes then churn back and forth across that
#      boundary, one row at a time, verified after each
#   3. a block remove tears a multi-page hole out of the middle
#
# A split or merge that mishandles the first or last row of a page shows up
# here and almost nowhere else.

BOUNDARY_PAGES = (1, 2, 3)
BOUNDARY_WINDOW = 12


def boundary_grow_to(h, var, target):
    """Insert or remove in one block so the variable ends up `target` rows long."""
    if target > len(var.rows):
        insert_at(h, var, len(var.rows), target - len(var.rows))
    elif target < len(var.rows):
        remove_at(h, var, target, len(var.rows))


def boundary_churn(h, var, boundary):
    """One small operation aimed within a row or two of the boundary.

    The length starts just short of the boundary, so aiming at the boundary
    means aiming at the end of the array; as inserts push the length past it,
    the same aim point moves inside the array. Either way the operation lands
    on the rows that share a page edge.
    """
    aim = max(0, min(boundary + h.rng.randint(-2, 2), len(var.rows)))

    if not var.rows or h.rng.random() < 0.5:
        insert_at(h, var, aim, h.rng.randint(1, 3))
    else:
        start = min(aim, len(var.rows) - 1)
        stop = min(start + h.rng.randint(1, 3), len(var.rows))
        remove_at(h, var, start, stop)


def simulate_page_boundaries(seed, steps, type_str, dtype, dims, path):
    """Block operations and single-row churn around whole-page boundaries."""
    rng = random.Random(seed)
    h = Harness(rng, path)

    var = Variable("v", type_str, dtype, dims)
    h.world.vars["v"] = var
    h.db.execute(f"create v {type_str}")

    per_page = rows_per_page(var)

    try:
        h.verify_var("v")
        for step in range(steps):
            h.step = step

            boundary = per_page * rng.choice(BOUNDARY_PAGES)

            # land just short of the boundary in one block, then verify
            boundary_grow_to(h, var, max(boundary - rng.randint(1, BOUNDARY_WINDOW), 0))
            h.verify_var("v")

            # walk across it a row or two at a time
            for _ in range(rng.randint(2, 6)):
                boundary_churn(h, var, boundary)
                h.verify_var("v")

            # occasionally tear a multi-page hole out of the middle
            if len(var.rows) > 2 * per_page and rng.random() < 0.3:
                start = rng.randint(1, len(var.rows) - 2 * per_page)
                remove_at(h, var, start, start + per_page + rng.randint(-2, 2))
                h.verify_var("v")

            # and prove the boundary survives a round trip to disk
            if rng.random() < 0.1:
                h.reopen()
                h.verify_var("v")

        h.reopen()
        h.verify_world()
    except Exception as exc:
        raise _report("boundary", seed, type_str, h, exc) from exc
    finally:
        h.close()


############# Simulation 3: create / get / delete across many variables
#
# One variable's schema operations must not disturb another's data, so this
# one keeps up to MAX_VARS live at once and re-verifies all of them after
# every step.

MAX_VARS = 8


def schema_create_new(h, txn=None):
    """A fresh name. Nothing else may change."""
    if len(h.world.vars) >= MAX_VARS:
        return schema_write_data(h, txn)

    name = f"v{h.rng.randrange(10**6)}"
    if name in h.world.vars:
        return

    type_str, dtype, dims = h.rng.choice(TYPE_POOL)
    h.target(txn).execute(f"create {name} {type_str}")
    h.log(f"create {name} {type_str}")

    h.world.vars[name] = Variable(name, type_str, dtype, dims)


def schema_create_existing_same_type(h, txn=None):
    """Re-creating with the identical type is a no-op, and must keep the data."""
    if not h.world.vars:
        return schema_create_new(h, txn)

    var = h.rng.choice(sorted(h.world.vars))
    var = h.world.vars[var]

    h.target(txn).execute(f"create {var.name} {var.type_str}")
    h.log(f"create {var.name} {var.type_str} (re-create, same type)")
    # the model is unchanged on purpose; verify_world checks the data survived


def schema_create_existing_other_type(h, txn=None):
    """Re-creating with a different type must be refused, and change nothing."""
    if not h.world.vars:
        return schema_create_new(h, txn)

    var = h.world.vars[h.rng.choice(sorted(h.world.vars))]
    other = [t for t in TYPE_POOL if t[0] != var.type_str]
    type_str = h.rng.choice(other)[0]

    h.log(f"create {var.name} {type_str} (expect refusal, is {var.type_str})")
    try:
        h.target(txn).execute(f"create {var.name} {type_str}")
    except RuntimeError:
        return

    raise Mismatch(
        f"create {var.name} {type_str} succeeded, but {var.name} is {var.type_str}"
    )


def schema_delete(h, txn=None):
    if not h.world.vars:
        return schema_create_new(h, txn)

    name = h.rng.choice(sorted(h.world.vars))

    h.target(txn).execute(f"delete {name}")
    h.log(f"delete {name}")

    del h.world.vars[name]


def schema_delete_missing(h, txn=None):
    name = f"gone{h.rng.randrange(10**6)}"
    if name in h.world.vars:
        return

    h.log(f"delete {name} (expect refusal)")
    try:
        h.target(txn).execute(f"delete {name}")
    except RuntimeError:
        return

    raise Mismatch(f"delete {name} succeeded, but it was never created")


def schema_get_missing(h, txn=None):
    name = f"gone{h.rng.randrange(10**6)}"
    if name in h.world.vars:
        return

    h.log(f"get {name} (expect refusal)")
    try:
        h.target(txn).get(f"get {name}")
    except RuntimeError:
        return

    raise Mismatch(f"get {name} succeeded, but it was never created")


def schema_delete_recreate(h, txn=None):
    """The name comes back; the data does not."""
    if not h.world.vars:
        return schema_create_new(h, txn)

    var = h.world.vars[h.rng.choice(sorted(h.world.vars))]
    type_str, dtype, dims = h.rng.choice(TYPE_POOL)

    target = h.target(txn)
    target.execute(f"delete {var.name}")
    target.execute(f"create {var.name} {type_str}")
    h.log(f"delete + create {var.name} as {type_str} (was {var.type_str})")

    h.world.vars[var.name] = Variable(var.name, type_str, dtype, dims)


def schema_write_data(h, txn=None):
    """Data churn on one variable, so the schema actions have something to lose."""
    if not h.world.vars:
        return schema_create_new(h, txn)

    var = h.world.vars[h.rng.choice(sorted(h.world.vars))]
    h.rng.choice([data_insert, data_insert, data_write, data_remove])(h, var, txn)


SCHEMA_ACTIONS = [
    (schema_create_new, 4),
    (schema_create_existing_same_type, 2),
    (schema_create_existing_other_type, 2),
    (schema_delete, 3),
    (schema_delete_missing, 1),
    (schema_get_missing, 1),
    (schema_delete_recreate, 2),
    (schema_write_data, 8),
]


def simulate_schema(seed, steps, path):
    """Many variables, create/get/delete interleaved with data churn."""
    rng = random.Random(seed)
    h = Harness(rng, path)

    pool = [a for a, w in SCHEMA_ACTIONS for _ in range(w)]

    try:
        for step in range(steps):
            h.step = step
            rng.choice(pool)(h)
            # every variable, every step: a schema action on one name must not
            # touch another name's rows
            h.verify_world()

            if rng.random() < 0.03:
                h.reopen()
                h.verify_world()

        h.reopen()
        h.verify_world()
    except Exception as exc:
        raise _report("schema", seed, "mixed", h, exc) from exc
    finally:
        h.close()


############# Simulation 4: space reclaimed by DELETE, and reused
#
# A deleted variable hands its pages back. The next variable to grow gets them,
# so a stale pointer or a double-freed page shows up as damage to a *different*
# variable than the one that was deleted.
#
# The witnesses here are filled once and never touched again, so any change to
# them is unambiguous corruption rather than a modelling mistake.

WITNESS_ROWS = 400
SCRATCH_PAGES = (1, 2, 5)


def simulate_reclaim(seed, steps, path):
    """Churn big variables through create/fill/delete, with fixed witnesses."""
    rng = random.Random(seed)
    h = Harness(rng, path)

    try:
        # two variables that must never change again
        for i, (type_str, dtype, dims) in enumerate([TYPE_POOL[0], TYPE_POOL[5]]):
            name = f"witness{i}"
            var = Variable(name, type_str, dtype, dims)
            h.world.vars[name] = var
            h.db.execute(f"create {name} {type_str}")
            insert_at(h, var, 0, WITNESS_ROWS)

        # and one that keeps growing, so its pages interleave with the churn
        grower_type, grower_dtype, grower_dims = TYPE_POOL[6]
        grower = Variable("grower", grower_type, grower_dtype, grower_dims)
        h.world.vars["grower"] = grower
        h.db.execute(f"create grower {grower_type}")

        h.verify_world()
        frozen = {
            name: h.world.vars[name].as_array().copy()
            for name in ("witness0", "witness1")
        }

        for step in range(steps):
            h.step = step

            # a scratch variable, filled with several pages, then dropped
            type_str, dtype, dims = rng.choice(TYPE_POOL)
            scratch = Variable(f"scratch{step}", type_str, dtype, dims)
            rows = rows_per_page(scratch) * rng.choice(SCRATCH_PAGES)

            h.world.vars[scratch.name] = scratch
            h.db.execute(f"create {scratch.name} {type_str}")
            insert_at(h, scratch, 0, rows)
            h.verify_var(scratch.name)

            h.db.execute(f"delete {scratch.name}")
            h.log(f"delete {scratch.name} ({rows} rows)")
            del h.world.vars[scratch.name]

            # the grower takes the freed pages
            rng.choice([data_insert, data_insert, data_write, data_remove])(h, grower)

            if rng.random() < 0.1:
                h.reopen()

            h.verify_world()

            # the witnesses are byte-for-byte what they were before any churn
            for name, expected in frozen.items():
                np.testing.assert_array_equal(
                    h.db.read(f"read {name}"),
                    expected,
                    err_msg=f"{name} changed while {scratch.name} churned",
                )

        h.reopen()
        h.verify_world()
    except Exception as exc:
        raise _report("reclaim", seed, "mixed", h, exc) from exc
    finally:
        h.close()


############# Simulation 5: many variables under data load at once
#
# Simulation 3 churns data lightly while it exercises create/get/delete. This
# one holds the schema still and puts every variable under heavy data load
# instead, so that the only thing left to break is one variable's rows showing
# up inside another's.

INTERLEAVED_TYPES = [
    TYPE_POOL[0],  # u8,       1 byte
    TYPE_POOL[1],  # u16,      2 bytes
    TYPE_POOL[3],  # i64,      8 bytes
    TYPE_POOL[5],  # f64,      8 bytes
    TYPE_POOL[6],  # [3]u32,  12 bytes
]
INTERLEAVED_CAP = 600


def simulate_interleaved(seed, steps, path):
    """Several variables of different widths, all mutating, all verified."""
    rng = random.Random(seed)
    h = Harness(rng, path)

    try:
        for i, (type_str, dtype, dims) in enumerate(INTERLEAVED_TYPES):
            name = f"v{i}_{type_str.replace('[', '').replace(']', '')}"
            h.world.vars[name] = Variable(name, type_str, dtype, dims)
            h.db.execute(f"create {name} {type_str}")

        mutations = [a for a, w in DATA_MUTATIONS for _ in range(w)]
        shrink = [data_remove, data_destructive_read]

        h.verify_world()
        for step in range(steps):
            h.step = step

            var = h.world.vars[rng.choice(sorted(h.world.names()))]

            # keep every variable in the same size band, so none of them
            # dominates the file and the others stay interleaved with it
            if len(var.rows) > INTERLEAVED_CAP:
                rng.choice(shrink)(h, var)
            else:
                rng.choice(mutations)(h, var)

            # every variable, every step: the one that moved and the four
            # that did not
            h.verify_world()

            if rng.random() < 0.02:
                h.reopen()
                h.verify_world()

        h.reopen()
        h.verify_world()
    except Exception as exc:
        raise _report("interleaved", seed, "mixed", h, exc) from exc
    finally:
        h.close()


############# Simulation 6: the same operations, inside transactions


def simulate_txn(seed, steps, type_str, dtype, dims, path):
    """Batches of data operations that either all land or all vanish.

    Nothing touches the database while the transaction is open -- that
    deadlocks by design -- so the batch runs entirely through the handle and
    the model only adopts the shadow copy once the commit returns.
    """
    rng = random.Random(seed)
    h = Harness(rng, path)

    h.world.vars["v"] = Variable("v", type_str, dtype, dims)
    h.db.execute(f"create v {type_str}")

    mutations = [a for a, w in DATA_MUTATIONS for _ in range(w)]

    try:
        for step in range(steps):
            h.step = step
            commit = rng.random() < 0.5
            shadow = h.world.copy()
            h.log(f"begin ({'commit' if commit else 'rollback'})")

            saved = h.world
            txn = h.db.begin()
            try:
                h.world = shadow
                for _ in range(rng.randint(1, 4)):
                    rng.choice(mutations)(h, shadow.vars["v"], txn)
                    # the transaction must see its own uncommitted work
                    h.verify_var("v", txn)
            except Exception:
                h.world = saved
                txn.rollback()
                raise

            if commit:
                txn.commit()
                h.world = shadow
            else:
                txn.rollback()
                h.world = saved

            h.verify_var("v")

        h.reopen()
        h.verify_world()
    except Exception as exc:
        raise _report("txn", seed, type_str, h, exc) from exc
    finally:
        h.close()


############# Failure reporting


def _report(kind, seed, type_str, h, exc):
    return AssertionError(
        f"{kind} simulation failed at step {h.step}: "
        f"seed={seed} type={type_str}\n"
        f"replay: python3 simulation.py --kind {kind} --seed {seed}\n"
        + "\n".join(h.journal[-40:])
        + f"\n\n{type(exc).__name__}: {exc}"
    )


############# The pytest-facing, fast version

STEPS = int(os.environ.get("NS_SIM_STEPS", "60"))
RUNS = int(os.environ.get("NS_SIM_RUNS", "3"))

# A boundary step grows the variable by a block and then churns and re-verifies
# it several times, and a reclaim step builds and drops a multi-page variable.
# Both are worth an order of magnitude more work than a plain data step, so
# they get a smaller budget to keep the quick suite quick. The soak runner
# gives every kind the full --steps.
HEAVY_STEPS = max(STEPS // 4, 5)


def _seeds(n):
    env = os.environ.get("NS_SIM_SEED")
    if env:
        return [int(env)] * n
    master = random.Random()
    return [master.randrange(2**32) for _ in range(n)]


@pytest.mark.parametrize(
    "type_str,dtype,dims", TYPE_POOL, ids=[t[0] for t in TYPE_POOL]
)
def test_data_operations(db_dir, type_str, dtype, dims):
    """insert / write / remove / read, verified after every mutation."""
    for seed in _seeds(RUNS):
        print(f"data seed={seed} type={type_str}")
        simulate_data(seed, STEPS, type_str, dtype, dims, f"data-{seed}.db")


@pytest.mark.parametrize(
    "type_str,dtype,dims", TYPE_POOL, ids=[t[0] for t in TYPE_POOL]
)
def test_page_boundary_operations(db_dir, type_str, dtype, dims):
    """Block operations and single-row churn across whole-page boundaries."""
    for seed in _seeds(RUNS):
        print(f"boundary seed={seed} type={type_str}")
        simulate_page_boundaries(
            seed, HEAVY_STEPS, type_str, dtype, dims, f"bnd-{seed}.db"
        )


def test_space_reclaimed_by_delete(db_dir):
    """Big variables churned through create/fill/delete, with fixed witnesses."""
    for seed in _seeds(RUNS):
        print(f"reclaim seed={seed}")
        simulate_reclaim(seed, HEAVY_STEPS, f"rec-{seed}.db")


def test_interleaved_variables(db_dir):
    """Five variables of different widths, all under data load at once."""
    for seed in _seeds(RUNS):
        print(f"interleaved seed={seed}")
        simulate_interleaved(seed, STEPS, f"int-{seed}.db")


def test_schema_operations(db_dir):
    """create / get / delete across many variables at once."""
    for seed in _seeds(RUNS):
        print(f"schema seed={seed}")
        simulate_schema(seed, STEPS, f"schema-{seed}.db")


_TXN_TYPES = [TYPE_POOL[0], TYPE_POOL[5], TYPE_POOL[6]]  # u8, f64, [3]u32


@pytest.mark.parametrize(
    "type_str,dtype,dims", _TXN_TYPES, ids=[t[0] for t in _TXN_TYPES]
)
def test_transactional_operations(db_dir, type_str, dtype, dims):
    """The same data operations, committed or rolled back in batches."""
    for seed in _seeds(RUNS):
        print(f"txn seed={seed} type={type_str}")
        simulate_txn(seed, STEPS, type_str, dtype, dims, f"txn-{seed}.db")


############# The standalone soak version

KINDS = ("data", "boundary", "schema", "reclaim", "interleaved", "txn")


# kinds that run once over mixed types, rather than once per type
MIXED_KINDS = {
    "schema": simulate_schema,
    "reclaim": simulate_reclaim,
    "interleaved": simulate_interleaved,
}
PER_TYPE_KINDS = {
    "data": simulate_data,
    "boundary": simulate_page_boundaries,
    "txn": simulate_txn,
}


def _run_one(kind, seed, steps, tag):
    path = f"soak-{kind}-{seed}-{tag}.db"
    try:
        if kind in MIXED_KINDS:
            # the reclaim simulation creates a fresh variable per step, so it
            # gets fewer steps for the same amount of work
            budget = max(steps // 4, 5) if kind == "reclaim" else steps
            MIXED_KINDS[kind](seed, budget, path)
        else:
            type_str, dtype, dims = next(t for t in TYPE_POOL if t[0] == tag)
            PER_TYPE_KINDS[kind](seed, steps, type_str, dtype, dims, path)
        return None
    finally:
        for suffix in ("", ".wal"):
            try:
                os.remove(path + suffix)
            except OSError:
                pass


def main(argv=None):
    ap = argparse.ArgumentParser(description="numstore operation simulations")
    ap.add_argument("--seed", type=int, help="replay one run")
    ap.add_argument("--kind", choices=KINDS, help="run only this simulation")
    ap.add_argument("--runs", type=int, default=20)
    ap.add_argument("--steps", type=int, default=200)
    ap.add_argument("--dir", default=".")
    args = ap.parse_args(argv)

    os.chdir(args.dir)
    kinds = [args.kind] if args.kind else list(KINDS)
    master = random.Random(args.seed)
    failures = 0

    for i in range(1 if args.seed else args.runs):
        seed = args.seed if args.seed else master.randrange(2**32)
        for kind in kinds:
            tags = ["mixed"] if kind in MIXED_KINDS else [t[0] for t in TYPE_POOL]
            for tag in tags:
                try:
                    _run_one(kind, seed, args.steps, tag)
                    print(f"ok   run {i} {kind:<6} seed={seed} {tag}")
                except AssertionError as exc:
                    failures += 1
                    print(f"FAIL run {i} {kind:<6} seed={seed} {tag}\n{exc}\n")

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
