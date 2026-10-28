"""Data roundtrips across the type system.

utils.py pins down the numstore-type -> numpy-dtype mapping, and var.py pins
down the metadata a GET reports. Neither one puts data through a variable of
each shape and reads it back, which is what this file does.
"""

from __future__ import annotations

import numpy as np
import pytest

import pynumstore as ns


def _cplx(part):
    return np.dtype([("re", part), ("im", part)])


class TestScalarRoundtrips:
    ############# Every integer width, at its limits
    @pytest.mark.parametrize(
        "code,dtype",
        [
            ("u8", np.uint8),
            ("u16", np.uint16),
            ("u32", np.uint32),
            ("u64", np.uint64),
            ("i8", np.int8),
            ("i16", np.int16),
            ("i32", np.int32),
            ("i64", np.int64),
        ],
    )
    def test_integer_limits_roundtrip(self, db, code, dtype):
        info = np.iinfo(dtype)
        src = np.array([info.min, 0, 1, info.max], dtype=dtype)

        db.execute(f"create foo {code}")
        assert db.write(f"insert foo 0 {src.size}", src) == src.size

        np.testing.assert_array_equal(db.read("read foo"), src)

    @pytest.mark.parametrize(
        "code,dtype", [("f16", np.float16), ("f32", np.float32), ("f64", np.float64)]
    )
    def test_float_specials_roundtrip(self, db, code, dtype):
        src = np.array([0.0, -0.0, 1.0, np.nan, np.inf, -np.inf], dtype=dtype)

        db.execute(f"create foo {code}")
        db.write(f"insert foo 0 {src.size}", src)

        out = db.read("read foo")
        np.testing.assert_array_equal(out, src)  # assert_array_equal treats nan == nan
        # -0.0 must not have been flattened to +0.0
        assert np.signbit(out[1])

    @pytest.mark.parametrize(
        "code,dtype",
        [
            ("cf64", np.complex64),
            ("cf128", np.complex128),
        ],
    )
    def test_native_complex_roundtrip(self, db, code, dtype):
        src = np.array([1 + 2j, -3.5 - 0.25j, 0j], dtype=dtype)

        db.execute(f"create foo {code}")
        db.write(f"insert foo 0 {src.size}", src)

        np.testing.assert_array_equal(db.read("read foo"), src)

    @pytest.mark.parametrize(
        "code,part",
        [
            ("cu16", np.uint8),
            ("cu32", np.uint16),
            ("ci32", np.int16),
            ("cf32", np.float16),
        ],
    )
    def test_structured_complex_roundtrip(self, db, code, part):
        dtype = _cplx(part)
        src = np.array([(1, 2), (3, 4)], dtype=dtype)

        db.execute(f"create foo {code}")
        db.write(f"insert foo 0 {src.size}", src)

        np.testing.assert_array_equal(db.read("read foo"), src)


class TestCompositeRoundtrips:
    ############# Structs
    def test_flat_struct_roundtrip(self, db):
        dtype = np.dtype([("a", np.uint32), ("b", np.float32)])
        src = np.array([(1, 1.5), (2, 2.5), (3, 3.5)], dtype=dtype)

        db.execute("create foo struct { a u32, b f32 }")
        db.write(f"insert foo 0 {src.size}", src)

        np.testing.assert_array_equal(db.read("read foo"), src)

    def test_struct_accepts_a_list_of_tuples(self, db):
        db.execute("create pt struct { x u8, y u8 }")

        db.write("insert pt 0 2", [(1, 2), (3, 4)])

        out = db.read("read pt")
        assert out["x"].tolist() == [1, 3]
        assert out["y"].tolist() == [2, 4]

    def test_nested_struct_roundtrip(self, db):
        dtype = np.dtype(
            [("i", np.uint8), ("b", [("a", np.uint32), ("b", np.float32)])]
        )
        src = np.array([(7, (1, 1.5)), (8, (2, 2.5))], dtype=dtype)

        db.execute("create foo struct { i u8, b struct { a u32, b f32 } }")
        db.write(f"insert foo 0 {src.size}", src)

        np.testing.assert_array_equal(db.read("read foo"), src)

    def test_struct_with_an_inner_array_roundtrip(self, db):
        dtype = np.dtype([("a", np.uint8), ("b", np.float32, (4,))])
        src = np.array(
            [(1, [1.0, 2.0, 3.0, 4.0]), (2, [5.0, 6.0, 7.0, 8.0])], dtype=dtype
        )

        db.execute("create foo struct { a u8, b [4]f32 }")
        db.write(f"insert foo 0 {src.size}", src)

        np.testing.assert_array_equal(db.read("read foo"), src)

    def test_struct_field_access_survives_a_remove(self, db):
        dtype = np.dtype([("a", np.uint32), ("b", np.float32)])
        src = np.array([(i, i + 0.5) for i in range(6)], dtype=dtype)

        db.execute("create foo struct { a u32, b f32 }")
        db.write("insert foo 0 6", src)
        db.execute("remove foo[0:2]")

        np.testing.assert_array_equal(db.read("read foo"), src[2:])

    ############# Unions
    def test_union_roundtrip_through_its_widest_member(self, db):
        db.execute("create foo union { a u32, b f64 }")
        dtype = db.get("get foo").dtype

        src = np.zeros(3, dtype=dtype)
        src["b"] = [1.5, 2.5, 3.5]
        db.write("insert foo 0 3", src)

        # The bytes are the bytes; reading back through 'b' gives them back
        np.testing.assert_array_equal(db.read("read foo")["b"], [1.5, 2.5, 3.5])

    def test_union_members_overlap(self, db):
        db.execute("create foo union { a u32, b f32 }")
        dtype = db.get("get foo").dtype

        src = np.zeros(1, dtype=dtype)
        src["a"] = 0x3F800000  # the f32 bit pattern for 1.0
        db.write("insert foo 0 1", src)

        assert db.read("read foo")["b"][0] == 1.0

    ############# Strict arrays
    @pytest.mark.parametrize(
        "type_str,dims",
        [
            ("[1]u32", (1,)),
            ("[3]u32", (3,)),
            ("[2][3]u32", (2, 3)),
            ("[2][3][4]u8", (2, 3, 4)),
        ],
    )
    def test_strict_array_roundtrip(self, db, type_str, dims):
        rows = 4
        src = np.arange(
            rows * int(np.prod(dims)), dtype=np.uint8 if "u8" in type_str else np.uint32
        )
        src = src.reshape(rows, *dims)

        db.execute(f"create grid {type_str}")
        assert db.write(f"insert grid 0 {rows}", src) == rows

        out = db.read("read grid")
        assert out.shape == (rows, *dims)
        np.testing.assert_array_equal(out, src)

    def test_strict_array_rejects_the_wrong_row_shape(self, db):
        db.execute("create grid [3]u32")

        with pytest.raises(ValueError):
            db.write("insert grid 0 2", np.zeros((2, 4), dtype=np.uint32))

    def test_strict_array_of_structs_roundtrip(self, db):
        dtype = np.dtype([("a", np.uint32), ("b", np.float32)])
        src = np.array([[(1, 1.5), (2, 2.5)], [(3, 3.5), (4, 4.5)]], dtype=dtype)

        db.execute("create foo [2]struct { a u32, b f32 }")
        db.write("insert foo 0 2", src)

        out = db.read("read foo")
        assert out.shape == (2, 2)
        np.testing.assert_array_equal(out, src)

    def test_a_strided_read_of_a_strict_array_keeps_whole_rows(self, db):
        src = np.arange(12, dtype=np.uint32).reshape(4, 3)
        db.execute("create grid [3]u32")
        db.write("insert grid 0 4", src)

        np.testing.assert_array_equal(db.read("read grid[0:4:2]"), src[::2])


class TestScale:
    """Sizes that have to cross page boundaries."""

    @pytest.mark.parametrize("n", [1, 1000, 100_000])
    def test_a_large_variable_roundtrips(self, db, n):
        src = np.arange(n, dtype=np.uint32)

        db.execute("create big u32")
        assert db.write(f"insert big 0 {n}", src) == n

        assert db.get("get big").length == n
        np.testing.assert_array_equal(db.read("read big"), src)

    def test_an_insert_in_the_middle_of_a_large_variable(self, db):
        n = 50_000
        src = np.arange(n, dtype=np.uint32)
        db.execute("create big u32")
        db.write(f"insert big 0 {n}", src)

        db.write("insert big 25000 1", np.array([7], dtype=np.uint32))

        assert db.get("get big").length == n + 1
        out = db.read("read big")
        np.testing.assert_array_equal(out[:25000], src[:25000])
        assert out[25000] == 7
        np.testing.assert_array_equal(out[25001:], src[25000:])

    def test_a_remove_from_the_middle_of_a_large_variable(self, db):
        n = 50_000
        src = np.arange(n, dtype=np.uint32)
        db.execute("create big u32")
        db.write(f"insert big 0 {n}", src)

        db.execute("remove big[10000:20000]")

        assert db.get("get big").length == n - 10000
        np.testing.assert_array_equal(
            db.read("read big"), np.concatenate([src[:10000], src[20000:]])
        )

    def test_many_variables(self, db):
        for i in range(200):
            db.execute(f"create v{i} u32")
            db.write(f"insert v{i} 0 1", np.array([i], dtype=np.uint32))

        assert len({v.name for v in db.get_all()}) == 200
        assert db.read("read v199")[0] == 199

    def test_a_large_variable_survives_a_reopen(self, db_dir):
        n = 20_000
        src = np.arange(n, dtype=np.uint32)

        db = ns.Database("big.db")
        db.execute("create big u32")
        db.write(f"insert big 0 {n}", src)
        db.close()

        db = ns.Database("big.db")
        try:
            np.testing.assert_array_equal(db.read("read big"), src)
        finally:
            db.close()
