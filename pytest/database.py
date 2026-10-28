from __future__ import annotations

from contextlib import nullcontext as does_not_raise

import numpy as np
import pytest

import pynumstore as ns


class Boom(Exception):
    """Raised inside a with-block, to check the block still cleans up."""


class TestDatabase:
    ############# Lifecycle
    def test_context_manager_closes(self, db_dir):
        with ns.Database("ctx.db") as db:
            db.execute("create foo u32")

        assert db._handle is None

    def test_double_close_is_idempotent(self, db_dir):
        db = ns.Database("close.db")
        db.close()

        # Doesn't crash on second close
        with does_not_raise():
            db.close()

    def test_execute_after_close_raises(self, db_dir):
        db = ns.Database("closed.db")
        db.close()

        # Cannot execute on a closed database
        with pytest.raises(RuntimeError):
            db.execute("create foo u32")

    def test_var_after_database_close_raises(self, db_dir):
        db = ns.Database("varclose.db")
        db.execute("create foo u32")
        db.close()

        with pytest.raises(RuntimeError):
            db.get("get foo")

    def test_get_all_on_a_closed_database_raises(self, db_dir):
        database = ns.Database("closed.db")
        database.execute("create foo u32")
        database.close()

        with pytest.raises(RuntimeError):
            database.get_all()

    ############# Persistence across close/reopen
    def test_committed_transaction_survives_reopen(self, db_dir):
        db = ns.Database("p.db")
        db.execute("create foo u32")
        with db.begin() as tx:
            tx.write("insert foo 0 2", np.array([4, 5], dtype=np.uint32))
        db.close()

        db = ns.Database("p.db")
        np.testing.assert_array_equal(db.read("read foo[0:]"), [4, 5])
        db.close()

    def test_rolled_back_transaction_does_not_survive_reopen(self, db_dir):
        db = ns.Database("p.db")
        db.execute("create foo u32")
        tx = db.begin()
        tx.write("insert foo 0 2", np.array([4, 5], dtype=np.uint32))
        tx.rollback()
        db.close()

        db = ns.Database("p.db")
        assert db.read("read foo[0:]").size == 0
        db.close()

    def test_reopen_without_closing_the_first_handle(self, db_dir):
        db = ns.Database("p.db")
        db.execute("create foo u32")
        db.write("insert foo 0 1", np.array([1], dtype=np.uint32))

        second = ns.Database("p.db")
        try:
            np.testing.assert_array_equal(second.read("read foo[0:]"), [1])
        finally:
            second.close()
            db.close()

    ############# Paths and construction
    def test_path_is_exposed_verbatim(self, db_dir):
        with ns.Database("some.db") as db:
            assert db.path == "some.db"

    def test_accepts_a_pathlib_path(self, db_dir):
        import pathlib

        target = db_dir / "sub.db"
        with ns.Database(pathlib.Path(target)) as db:
            db.execute("create foo u32")
            assert db.path == str(target)

    def test_a_path_in_a_missing_directory_raises(self, db_dir):
        with pytest.raises((RuntimeError, OSError)):
            ns.Database("no_such_dir/x.db")

    def test_a_directory_as_a_path_raises(self, db_dir):
        (db_dir / "adir").mkdir()

        with pytest.raises((RuntimeError, OSError)):
            ns.Database("adir")

    def test_an_empty_path_raises(self, db_dir):
        with pytest.raises((RuntimeError, OSError, ValueError)):
            ns.Database("")

    def test_repr_tracks_the_open_state(self, db_dir):
        db = ns.Database("r.db")
        assert repr(db) == "<Database 'r.db'>"

        db.close()
        assert repr(db) == "<Database 'r.db' closed>"

    def test_closed_property_tracks_close(self, db_dir):
        db = ns.Database("c.db")
        assert db.closed is False

        db.close()
        assert db.closed is True

    def test_context_manager_closes_on_an_exception(self, db_dir):
        db = None
        with pytest.raises(Boom), ns.Database("exc.db") as handle:
            db = handle
            raise Boom

        assert db is not None
        assert db.closed is True

    def test_begin_on_a_closed_database_raises(self, db_dir):
        db = ns.Database("b.db")
        db.close()

        with pytest.raises(RuntimeError, match="database is closed"):
            db.begin()

    def test_reopening_the_same_path_many_times(self, db_dir):
        for i in range(10):
            db = ns.Database("reopen.db")
            try:
                db.execute(f"create v{i} u32")
                assert len(db.get_all()) == i + 1
            finally:
                db.close()

    def test_two_databases_in_one_process_are_independent(self, db_dir):
        a = ns.Database("a.db")
        b = ns.Database("b.db")
        try:
            a.execute("create foo u32")
            b.execute("create bar f64")

            assert {v.name for v in a.get_all()} == {"foo"}
            assert {v.name for v in b.get_all()} == {"bar"}
        finally:
            a.close()
            b.close()
