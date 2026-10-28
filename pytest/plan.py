from __future__ import annotations

import numpy as np
import pytest

import pynumstore as ns


class Boom(Exception):
    """Raised inside a with-block, to check the block still cleans up."""


@pytest.fixture
def foo(db):
    """A u32 variable holding [0, 1, 2, 3, 4, 5]."""
    db.execute("create foo u32")
    db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))
    return db


class TestPlan:
    ############# Shape of a plan
    def test_plan_basic_accessors(self, foo):
        plan = foo.prepare("read foo[0:]")

        assert plan.query == "read foo[0:]"
        assert plan.closed is False
        assert repr(plan) == "<Plan 'read foo[0:]'>"

        plan.close()

        assert plan.closed is True
        assert repr(plan) == "<Plan 'read foo[0:]' closed>"

    def test_plan_var_matches_get(self, foo):
        with foo.prepare("get foo") as plan:
            assert plan.var() == foo.get("get foo")
            assert plan.var() == ns.Var("foo", "u32", 6, 4)

    ############# Reuse: the whole point of preparing
    def test_a_read_plan_can_run_many_times(self, foo):
        with foo.prepare("read foo[0:3]") as plan:
            for _ in range(3):
                np.testing.assert_array_equal(plan.read(), [0, 1, 2])

    def test_a_plan_sees_writes_made_after_it_was_prepared(self, foo):
        with foo.prepare("read foo[0:]") as plan:
            np.testing.assert_array_equal(plan.read(), np.arange(6))

            foo.write("insert foo 6 2", np.array([6, 7], dtype=np.uint32))

            # The plan is a compiled query, not a snapshot of the result
            np.testing.assert_array_equal(plan.read(), np.arange(8))

    def test_a_plan_sees_removes_made_after_it_was_prepared(self, foo):
        with foo.prepare("get foo") as plan:
            assert plan.var().length == 6

            foo.execute("remove foo[0:2]")

            assert plan.var().length == 4

    def test_an_insert_plan_appends_each_time_it_runs(self, db):
        db.execute("create foo u32")

        with db.prepare("insert foo 0 2") as plan:
            plan.write([1, 2])
            plan.write([3, 4])
            plan.write([5, 6])

        # Each run inserts at offset 0, so the last write ends up first
        np.testing.assert_array_equal(db.read("read foo"), [5, 6, 3, 4, 1, 2])

    def test_a_create_plan_run_twice_is_idempotent(self, db):
        with db.prepare("create foo u32") as plan:
            plan.execute()
            plan.execute()

        assert db.get("get foo") == ns.Var("foo", "u32", 0, 4)

    def test_read_into_reuses_one_buffer(self, foo):
        out = np.zeros(6, dtype=np.uint32)

        with foo.prepare("read foo[0:]") as plan:
            assert plan.read_into(out) == 6
            np.testing.assert_array_equal(out, np.arange(6))

            out[:] = 0
            assert plan.read_into(out) == 6
            np.testing.assert_array_equal(out, np.arange(6))

    def test_many_plans_on_one_database_are_independent(self, foo):
        read = foo.prepare("read foo[0:2]")
        get = foo.prepare("get foo")
        remove = foo.prepare("remove foo[0:1]")

        try:
            np.testing.assert_array_equal(read.read(), [0, 1])
            assert get.var().length == 6

            remove.execute()

            assert get.var().length == 5
            np.testing.assert_array_equal(read.read(), [1, 2])
        finally:
            for plan in (read, get, remove):
                plan.close()

    ############# Preparing is compiling, not resolving
    @pytest.mark.parametrize(
        "query",
        [
            pytest.param("not a real query", id="garbage"),
            pytest.param("", id="empty"),
            pytest.param("   ", id="whitespace"),
            pytest.param("create", id="verb-only"),
            pytest.param("create foo notatype", id="unknown-type"),
            pytest.param("read foo[a:b]", id="non-numeric-slice"),
            pytest.param("read foo[0:", id="unterminated-slice"),
        ],
    )
    def test_preparing_a_malformed_query_raises(self, db, query):
        with pytest.raises(RuntimeError):
            db.prepare(query)

    def test_preparing_an_unknown_variable_succeeds(self, db):
        """Name resolution is the database's job at run time, not the compiler's."""
        plan = db.prepare("read nope[0:]")
        try:
            with pytest.raises(RuntimeError):
                plan.read()
        finally:
            plan.close()

    def test_a_plan_prepared_before_its_variable_exists_still_runs(self, db):
        plan = db.prepare("read foo[0:]")
        try:
            db.execute("create foo u32")
            db.write("insert foo 0 2", np.array([1, 2], dtype=np.uint32))

            np.testing.assert_array_equal(plan.read(), [1, 2])
        finally:
            plan.close()

    ############# Verb mismatches
    def test_running_a_plan_with_the_wrong_method_raises(self, foo):
        with foo.prepare("read foo[0:]") as plan, pytest.raises(RuntimeError):
            plan.write([1])

        with foo.prepare("insert foo 0 1") as plan, pytest.raises(RuntimeError):
            plan.read()

        with foo.prepare("get foo") as plan, pytest.raises(RuntimeError):
            plan.execute()

    ############# Lifecycle
    def test_double_close_is_idempotent(self, foo):
        plan = foo.prepare("read foo[0:]")
        plan.close()
        plan.close()

        assert plan.closed is True

    @pytest.mark.parametrize(
        "run",
        [
            pytest.param(lambda p: p.execute(), id="execute"),
            pytest.param(lambda p: p.read(), id="read"),
            pytest.param(lambda p: p.read_into(np.zeros(6, np.uint32)), id="read_into"),
            pytest.param(lambda p: p.write([1]), id="write"),
            pytest.param(lambda p: p.var(), id="var"),
        ],
    )
    def test_using_a_closed_plan_raises(self, foo, run):
        plan = foo.prepare("read foo[0:]")
        plan.close()

        with pytest.raises(RuntimeError, match="plan is closed"):
            run(plan)

    @pytest.mark.parametrize(
        "run",
        [
            pytest.param(lambda p: p.execute(), id="execute"),
            pytest.param(lambda p: p.read(), id="read"),
            pytest.param(lambda p: p.read_into(np.zeros(6, np.uint32)), id="read_into"),
            pytest.param(lambda p: p.write([1]), id="write"),
            pytest.param(lambda p: p.var(), id="var"),
        ],
    )
    def test_using_a_plan_after_its_database_closed_raises(self, db_dir, run):
        db = ns.Database("p.db")
        db.execute("create foo u32")
        db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))
        plan = db.prepare("read foo[0:]")
        db.close()

        with pytest.raises(RuntimeError, match="database is closed"):
            run(plan)

    def test_a_plan_is_closed_once_its_database_is(self, db_dir):
        db = ns.Database("p.db")
        plan = db.prepare("create foo u32")

        assert plan.closed is False
        db.close()
        assert plan.closed is True

    def test_closing_a_plan_after_its_database_is_fine(self, db_dir):
        db = ns.Database("p.db")
        plan = db.prepare("create foo u32")
        db.close()

        plan.close()

    def test_prepare_on_a_closed_database_raises(self, db_dir):
        db = ns.Database("p.db")
        db.close()

        with pytest.raises(RuntimeError, match="database is closed"):
            db.prepare("create foo u32")

    def test_context_manager_closes_on_an_exception(self, foo):
        plan = None
        with pytest.raises(Boom), foo.prepare("read foo[0:]") as p:
            plan = p
            raise Boom

        assert plan is not None
        assert plan.closed is True

    ############# Plans and transactions
    def test_a_plan_runs_inside_a_transaction(self, db):
        db.execute("create foo u32")

        with db.prepare("insert foo 0 2") as plan, db.begin() as txn:
            plan.write([7, 8], txn)

        np.testing.assert_array_equal(db.read("read foo"), [7, 8])

    def test_a_rolled_back_plan_write_leaves_nothing(self, db):
        db.execute("create foo u32")

        with db.prepare("insert foo 0 2") as plan:
            txn = db.begin()
            plan.write([7, 8], txn)
            txn.rollback()

        assert db.read("read foo").size == 0

    def test_a_plan_with_a_foreign_transaction_raises(self, db_dir):
        a = ns.Database("a.db")
        b = ns.Database("b.db")
        try:
            a.execute("create foo u32")
            plan = a.prepare("read foo[0:]")
            txn = b.begin()

            with pytest.raises(ValueError, match="different database"):
                plan.read(txn)

            txn.rollback()
            plan.close()
        finally:
            a.close()
            b.close()

    def test_a_plan_with_a_finished_transaction_raises(self, foo):
        with foo.prepare("read foo[0:]") as plan:
            txn = foo.begin()
            txn.commit()

            with pytest.raises(RuntimeError, match="already committed or rolled back"):
                plan.read(txn)

    ############# Argument checking, same rules as the one-off methods
    def test_read_into_the_wrong_dtype_raises(self, foo):
        with foo.prepare("read foo[0:]") as plan, pytest.raises(TypeError):
            plan.read_into(np.zeros(6, dtype=np.float64))

    def test_read_into_a_noncontiguous_buffer_raises(self, foo):
        with foo.prepare("read foo[0:]") as plan, pytest.raises(ValueError):
            plan.read_into(np.zeros(12, dtype=np.uint32)[::2])

    def test_read_into_a_list_raises(self, foo):
        with foo.prepare("read foo[0:]") as plan, pytest.raises(TypeError):
            plan.read_into([0] * 6)

    def test_write_a_wider_dtype_raises(self, db):
        db.execute("create foo u32")

        with db.prepare("insert foo 0 1") as plan, pytest.raises(TypeError):
            plan.write(np.array(["x"]))

    def test_read_of_an_empty_variable_is_empty(self, db):
        db.execute("create foo u32")

        with db.prepare("read foo[0:]") as plan:
            out = plan.read()
            assert out.shape == (0,)
            assert out.dtype == np.uint32

    def test_a_strict_array_plan_keeps_its_shape(self, db):
        db.execute("create grid [3]u32")
        db.write("insert grid 0 2", np.arange(6, dtype=np.uint32).reshape(2, 3))

        with db.prepare("read grid[0:]") as plan:
            assert plan.read().shape == (2, 3)
            assert plan.var().shape == (2, 3)
