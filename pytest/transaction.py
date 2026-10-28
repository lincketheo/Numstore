from __future__ import annotations

import numpy as np
import pytest

import pynumstore as ns


class TestTransaction:
    ############# Commit / rollback basics
    def test_transaction_commit(self, db):
        db.execute("create foo u32")
        src = np.arange(3, dtype=np.uint32)

        with db.begin() as txn:
            txn.write(f"insert foo 0 {src.size}", src)

        dest = np.zeros(3, dtype=np.uint32)
        n = db.read_into(f"read foo[0:] blimit {dest.nbytes}", dest)
        assert n == dest.size
        np.testing.assert_array_equal(dest, src)

    def test_transaction_rollback(self, db):
        db.execute("create foo u32")
        src = np.arange(3, dtype=np.uint32)

        txn = db.begin()
        txn.write(f"insert foo 0 {src.size}", src)
        txn.rollback()

        dest = np.zeros(3, dtype=np.uint32)
        n = db.read_into(f"read foo[0:] blimit {dest.nbytes}", dest)
        assert n == 0

    def test_reusing_closed_transaction_raises(self, db):
        db.execute("create foo u32")

        txn = db.begin()
        txn.commit()

        with pytest.raises(RuntimeError):
            txn.read_into("read foo[0:] blimit 4", np.zeros(1, dtype=np.uint32))

    def test_double_commit_raises(self, db):
        txn = db.begin()
        txn.commit()

        with pytest.raises(RuntimeError):
            txn.commit()

    def test_double_rollback_raises(self, db):
        txn = db.begin()
        txn.rollback()

        with pytest.raises(RuntimeError):
            txn.rollback()

    def test_rollback_after_commit_raises(self, db):
        txn = db.begin()
        txn.commit()

        with pytest.raises(RuntimeError):
            txn.rollback()

    def test_commit_after_rollback_raises(self, db):
        txn = db.begin()
        txn.rollback()

        with pytest.raises(RuntimeError):
            txn.commit()

    def test_with_block_over_a_hand_committed_transaction_is_fine(self, db):
        db.execute("create foo u32")

        # Leaving the block is not a second commit - it has nothing left to do
        with db.begin() as txn:
            txn.write("insert foo 0 3", np.arange(3, dtype=np.uint32))
            txn.commit()

        np.testing.assert_array_equal(
            db.read("read foo[0:]"), np.arange(3, dtype=np.uint32)
        )

    def test_with_block_over_a_hand_rolled_back_transaction_is_fine(self, db):
        db.execute("create foo u32")

        with db.begin() as txn:
            txn.write("insert foo 0 3", np.arange(3, dtype=np.uint32))
            txn.rollback()

        assert db.get("get foo").length == 0

    def test_var_in_transaction(self, db):
        db.execute("create foo u32")
        src = np.arange(4, dtype=np.uint32)

        with db.begin() as txn:
            txn.write(f"insert foo 0 {src.size}", src)
            assert txn.get("get foo").length == src.size

        assert db.get("get foo").length == src.size

    ############# Visibility
    def test_uncommitted_write_is_visible_inside_its_transaction(self, db):
        db.execute("create foo u32")

        with db.begin() as tx:
            tx.write("insert foo 0 2", np.array([7, 8], dtype=np.uint32))
            np.testing.assert_array_equal(tx.read("read foo[0:]"), [7, 8])

    def test_rollback_discards_the_write(self, db):
        db.execute("create foo u32")

        tx = db.begin()
        tx.write("insert foo 0 2", np.array([7, 8], dtype=np.uint32))
        tx.rollback()

        assert db.read("read foo[0:]").size == 0

    ############# get_all
    def test_get_all_in_a_transaction(self, db):
        db.execute("create foo u32")

        with db.begin() as tx:
            tx.execute("create bar f64")
            assert {v.name for v in tx.get_all()} == {"foo", "bar"}

        assert {v.name for v in db.get_all()} == {"foo", "bar"}

    def test_get_all_after_rollback(self, db):
        db.execute("create foo u32")

        tx = db.begin()
        tx.execute("create bar f64")
        tx.rollback()

        assert {v.name for v in db.get_all()} == {"foo"}

    def test_get_all_on_a_finished_transaction_raises(self, db):
        tx = db.begin()
        tx.commit()

        with pytest.raises(RuntimeError):
            tx.get_all()

    ############# remove
    def test_get_length_after_a_remove_in_a_transaction(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))

        with db.begin() as txn:
            txn.execute("remove foo[0:3]")
            assert txn.get("get foo").length == 3

        assert db.get("get foo").length == 3

    def test_a_rolled_back_remove_restores_the_length(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))

        txn = db.begin()
        txn.execute("remove foo[0:3]")
        txn.rollback()

        reported = db.get("get foo").length
        buf = np.zeros(max(reported, 1) * 2 + 8, dtype=np.uint32)
        assert (reported, db.read_into("read foo", buf)) == (6, 6)
        np.testing.assert_array_equal(db.read("read foo"), np.arange(6))
