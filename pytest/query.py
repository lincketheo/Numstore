from __future__ import annotations

import dataclasses

import numpy as np
import pytest

import pynumstore as ns


def _length_matches(db, name, dtype):
    """GET's length, and the number of elements actually there."""
    reported = db.get(f"get {name}").length
    buf = np.zeros(max(reported, 1) * 2 + 8, dtype=dtype)
    return reported, db.read_into(f"read {name}", buf)


@pytest.fixture
def foo(db):
    """A u32 variable holding [0, 1, 2, 3, 4, 5]."""
    db.execute("create foo u32")
    db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))
    return db


class TestQuery:
    ############# Malformed queries
    @pytest.mark.parametrize(
        "query",
        [
            pytest.param("not a real query", id="garbage"),
            pytest.param("", id="empty"),
            pytest.param("   ", id="whitespace"),
            pytest.param("create", id="verb-only"),
            pytest.param("create foo", id="missing-type"),
            pytest.param("create foo notatype", id="unknown-type"),
            pytest.param("frobnicate foo", id="unknown-verb"),
            pytest.param("get", id="get-no-target"),
            pytest.param("read foo[0:", id="unterminated-slice"),
            pytest.param("read foo[a:b]", id="non-numeric-slice"),
            pytest.param("insert foo", id="insert-missing-args"),
            pytest.param("remove", id="remove-no-target"),
        ],
    )
    def test_invalid_query_raises(self, db, query):
        with pytest.raises(RuntimeError):
            db.execute(query)

        with pytest.raises(RuntimeError):
            db.read(query)

        with pytest.raises(RuntimeError):
            db.write(query, [1])

        with pytest.raises(RuntimeError):
            db.get(query)

    def test_invalid_query_types(self, db):
        db.execute("create foo u32")

        # get works on foo, but get() is not a verb a mutation query understands
        db.get("get foo")
        with pytest.raises(RuntimeError):
            db.execute("get foo")

        db.write("insert foo 0 5", np.arange(5, dtype=np.uint32))
        with pytest.raises(RuntimeError):
            db.read("insert foo 0 10")

        db.write("write foo[0:5]", np.arange(5, dtype=np.uint32))
        with pytest.raises(RuntimeError):
            db.read("write foo[0:5]")

        db.read("read foo[0:10]")
        with pytest.raises(RuntimeError):
            db.write("read foo[0:10]", np.zeros(1, dtype=np.uint32))

        db.read("remove foo[0:10]")
        with pytest.raises(RuntimeError):
            db.write("remove foo[0:10]", np.zeros(1, dtype=np.uint32))

    ############# create validation
    @pytest.mark.parametrize(
        "vtype",
        [
            "",
            "u7",
            "cu8",
            "x32",
            "[0]u32",
            "[x]u32",
            "[10]",
            "struct { }",
            "struct { a u8, a u8 }",
            # "struct { a u8", "u32 junk",
        ],
    )
    def test_create_bad_type(self, db, vtype):
        with pytest.raises(RuntimeError):
            db.execute(f"create foo {vtype}")
        # the failed create cannot have left a variable behind
        with pytest.raises(RuntimeError):
            db.get("get foo")

    @pytest.mark.parametrize("vname", ["1foo", "foo-bar", "[10]", "{"])
    def test_create_bad_name(self, db, vname):
        with pytest.raises(RuntimeError):
            db.execute(f"create {vname} u32")

    def test_create_duplicate(self, db):
        db.execute("create foo u32")

        # This is not ok - it's a different type
        with pytest.raises(RuntimeError):
            db.execute("create foo f64")

        # This is ok - it's the same type
        db.execute("create foo u32")
        assert db.get("get foo") == ns.Var("foo", "u32", 0, 4)

    def test_get_nonexistent_variable_fails(self, db):
        with pytest.raises(RuntimeError):
            db.get("get foo")

    ############# CRUD basics
    def test_create_insert_read(self, db):
        db.execute("create foo u32")

        src = np.arange(5, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        dest = np.zeros(5, dtype=np.uint32)
        n = db.read_into(f"read foo[0:] blimit {dest.nbytes}", dest)

        assert n == dest.size
        np.testing.assert_array_equal(dest, src)

    def test_write_returns_the_count_written(self, db):
        db.execute("create foo u32")
        src = np.arange(3, dtype=np.uint32)
        assert db.write(f"insert foo 0 {src.size}", src) == src.size

    def test_sarray_variable_accepts_matching_2d_array(self, db):
        db.execute("create grid [3] u32")

        src = np.arange(6, dtype=np.uint32).reshape(2, 3)
        assert db.write("insert grid 0 2", src) == 2
        np.testing.assert_array_equal(db.read("read grid[0:]"), src)

    def test_insert_into_nonexistent_variable_raises(self, db):
        with pytest.raises(RuntimeError):
            db.write("insert nope 0 1", np.zeros(1, dtype=np.uint32))

    def test_read_allocates_an_array(self, db):
        db.execute("create foo u32")
        src = np.arange(5, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        result = db.read("read foo[0:]")

        assert isinstance(result, np.ndarray)
        assert result.dtype == np.uint32
        np.testing.assert_array_equal(result, src)

    def test_read_into_returns_the_count_read(self, db):
        db.execute("create foo u32")
        src = np.arange(3, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        dest = np.zeros(3, dtype=np.uint32)
        assert db.read_into(f"read foo[0:] blimit {dest.nbytes}", dest) == 3

    def test_read_into_is_bounded_by_the_buffer_without_blimit(self, db):
        db.execute("create foo u32")
        src = np.arange(10, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        dest = np.zeros(4, dtype=np.uint32)
        # no blimit, so read_into cannot write past the buffer it was given
        assert db.read_into("read foo[0:]", dest) == dest.size
        np.testing.assert_array_equal(dest, src[:4])

    def test_strided_read_returns_every_nth_element(self, db):
        db.execute("create foo u32")
        src = np.arange(10, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        result = db.read("read foo[0::2]")

        np.testing.assert_array_equal(result, src[::2])

    def test_ranged_read_is_half_open(self, db):
        db.execute("create foo u32")
        src = np.arange(10, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        result = db.read("read foo[2:5]")

        np.testing.assert_array_equal(result, src[2:5])

    def test_read_of_an_empty_variable_is_empty(self, db):
        db.execute("create foo u32")

        result = db.read("read foo[0:]")

        assert result.size == 0

    def test_write_accepts_a_plain_python_list(self, db):
        db.execute("create foo f64")

        db.write("insert foo 0 3", [1.5, 2.25, 3.75])

        np.testing.assert_array_equal(db.read("read foo[0:]"), [1.5, 2.25, 3.75])

    def test_f64_roundtrips_without_precision_loss(self, db):
        db.execute("create foo f64")
        src = np.array([np.pi, -1e308, 2.2250738585072014e-308], dtype=np.float64)
        db.write(f"insert foo 0 {src.size}", src)

        np.testing.assert_array_equal(db.read("read foo[0:]"), src)

    def test_insert_in_the_middle_shifts_later_elements(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 4", np.array([0, 1, 2, 3], dtype=np.uint32))

        db.write("insert foo 2 1", np.array([99], dtype=np.uint32))

        np.testing.assert_array_equal(db.read("read foo[0:]"), [0, 1, 99, 2, 3])

    def test_read_past_the_end_raises(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 2", np.array([1, 2], dtype=np.uint32))

        with pytest.raises(Exception):
            db.read("read foo[5:9]")

    def test_insert_with_differing_counts_goes_with_smallest(self, db):
        db.execute("create foo u32")
        assert db.write("insert foo 0 5", np.array([1, 2], dtype=np.uint32)) == 2
        assert db.write("insert foo 0 1", np.array([1, 2], dtype=np.uint32)) == 1

    ############# Boundary and numeric-limit edge cases
    @pytest.mark.xfail(
        reason="zero-length operations are rejected by the query layer, not "
        "treated as no-ops. See the group of xfails in this file.",
        strict=True,
    )
    def test_zero_length_insert_writes_nothing(self, db):
        db.execute("create foo u32")

        assert db.write("insert foo 0 0", np.zeros(0, dtype=np.uint32)) == 0
        assert db.read("read foo[0:]").size == 0

    @pytest.mark.xfail(
        reason="zero-length operations are rejected by the query layer, not "
        "treated as no-ops. See the group of xfails in this file.",
        strict=True,
    )
    def test_read_an_empty_range_at_the_end(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 3", np.arange(3, dtype=np.uint32))

        assert db.read("read foo[3:3]").size == 0

    def test_read_a_single_element_range(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 5", np.arange(5, dtype=np.uint32))

        np.testing.assert_array_equal(db.read("read foo[4:5]"), [4])

    def test_stride_larger_than_length_yields_only_the_first(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 4", np.arange(4, dtype=np.uint32))

        np.testing.assert_array_equal(db.read("read foo[0::100]"), [0])

    def test_stride_equal_to_length(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 4", np.arange(4, dtype=np.uint32))

        np.testing.assert_array_equal(db.read("read foo[0::4]"), [0])

    def test_stride_of_one_is_every_element(self, db):
        db.execute("create foo u32")
        src = np.arange(4, dtype=np.uint32)
        db.write("insert foo 0 4", src)

        np.testing.assert_array_equal(db.read("read foo[0::1]"), src)

    def test_stride_of_zero_raises(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 4", np.arange(4, dtype=np.uint32))

        with pytest.raises(Exception):
            db.read("read foo[0::0]")

    def test_u8_holds_its_maximum(self, db):
        db.execute("create foo u8")
        src = np.array([0, 255], dtype=np.uint8)
        db.write("insert foo 0 2", src)

        np.testing.assert_array_equal(db.read("read foo[0:]"), src)

    def test_u64_holds_its_maximum(self, db):
        db.execute("create foo u64")
        src = np.array([2**64 - 1], dtype=np.uint64)
        db.write("insert foo 0 1", src)

        np.testing.assert_array_equal(db.read("read foo[0:]"), src)

    def test_i64_holds_its_minimum(self, db):
        db.execute("create foo i64")
        src = np.array([-(2**63)], dtype=np.int64)
        db.write("insert foo 0 1", src)

        np.testing.assert_array_equal(db.read("read foo[0:]"), src)

    def test_f64_roundtrips_nan_and_infinities(self, db):
        db.execute("create foo f64")
        src = np.array([np.nan, np.inf, -np.inf], dtype=np.float64)
        db.write("insert foo 0 3", src)

        result = db.read("read foo[0:]")
        # nan != nan, so it needs its own check instead of assert_array_equal
        assert np.isnan(result[0])
        assert result[1] == np.inf
        assert result[2] == -np.inf

    def test_append_at_exactly_the_end(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 3", np.arange(3, dtype=np.uint32))

        db.write("insert foo 3 1", np.array([9], dtype=np.uint32))

        np.testing.assert_array_equal(db.read("read foo[0:]"), [0, 1, 2, 9])

    @pytest.mark.xfail(
        reason="out-of-range positions clamp instead of raising: insert at 10 "
        "into a 2-element variable appends, giving length 3 and [0 1 9]. Same "
        "clamping as an oversized insert count.",
        strict=True,
    )
    def test_insert_leaving_a_gap_past_the_end_raises(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 2", np.arange(2, dtype=np.uint32))

        with pytest.raises(Exception):
            db.write("insert foo 10 1", np.array([9], dtype=np.uint32))

    def test_removing_everything_leaves_an_empty_variable(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 3", np.arange(3, dtype=np.uint32))

        db.read("remove foo[0:3]")

        assert db.read("read foo[0:]").size == 0

    @pytest.mark.xfail(
        reason="zero-length operations are rejected by the query layer, not "
        "treated as no-ops. See the group of xfails in this file.",
        strict=True,
    )
    def test_removing_an_empty_range_changes_nothing(self, db):
        db.execute("create foo u32")
        src = np.arange(3, dtype=np.uint32)
        db.write("insert foo 0 3", src)

        db.read("remove foo[1:1]")

        np.testing.assert_array_equal(db.read("read foo[0:]"), src)

    @pytest.mark.xfail(
        reason="zero-length operations are rejected by the query layer, not "
        "treated as no-ops. See the group of xfails in this file.",
        strict=True,
    )
    def test_read_into_a_zero_sized_buffer_reads_nothing(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 3", np.arange(3, dtype=np.uint32))

        assert db.read_into("read foo[0:]", np.zeros(0, dtype=np.uint32)) == 0

    def test_read_into_an_oversized_buffer_leaves_the_tail_untouched(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 2", np.array([1, 2], dtype=np.uint32))

        dest = np.full(5, 77, dtype=np.uint32)
        assert db.read_into("read foo[0:]", dest) == 2
        # read_into cannot have touched bytes past what it reported reading
        np.testing.assert_array_equal(dest[2:], [77, 77, 77])

    def test_read_into_the_wrong_dtype_raises(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 2", np.array([1, 2], dtype=np.uint32))

        with pytest.raises(TypeError):
            db.read_into("read foo[0:]", np.zeros(2, dtype=np.float64))

    def test_read_into_a_noncontiguous_buffer_raises(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 2", np.array([1, 2], dtype=np.uint32))

        with pytest.raises(ValueError):
            db.read_into("read foo[0:]", np.zeros(10, dtype=np.uint32)[::2])

    def test_read_into_a_readonly_buffer_raises(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 2", np.array([1, 2], dtype=np.uint32))

        dest = np.zeros(2, dtype=np.uint32)
        dest.flags.writeable = False
        with pytest.raises(ValueError):
            db.read_into("read foo[0:]", dest)

    def test_read_into_a_list_raises(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 2", np.array([1, 2], dtype=np.uint32))

        # read_into needs a buffer it can write into, not a value to copy
        with pytest.raises(TypeError):
            db.read_into("read foo[0:]", [0, 0])

    ############# remove, and the length-doubling bug
    #
    # numstore_remove and numstore_remove_malloc both copied insert's size update,
    # where `len` is the number of elements moved. In remove, `len` is the length
    # the array started with, so `nbytes + len * tsize` stored exactly twice the
    # real size and GET reported 2x the pre-remove length while the data itself
    # stayed correct.
    def test_read_of_a_remove_returns_the_removed_data(self, db):
        db.execute("create foo u32")
        src = np.arange(5, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        removed = db.read("remove foo[0:2]")
        np.testing.assert_array_equal(removed, src[0:2])

        remaining = db.read("read foo[0:]")
        np.testing.assert_array_equal(remaining, src[2:])

    def test_get_length_after_a_partial_remove(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 10", np.arange(10, dtype=np.uint32))

        db.execute("remove foo[0:2]")

        assert _length_matches(db, "foo", np.uint32) == (8, 8)

    def test_get_length_is_not_doubled_by_a_remove(self, db):
        """The exact shape of the bug: the stored size was nbytes * 2."""
        db.execute("create foo u32")
        db.write("insert foo 0 10", np.arange(10, dtype=np.uint32))

        db.execute("remove foo[0:1]")

        assert db.get("get foo").length == 9

    def test_get_length_after_removing_everything(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))

        db.execute("remove foo")

        assert _length_matches(db, "foo", np.uint32) == (0, 0)
        assert db.read("read foo").size == 0

    def test_get_length_after_a_strided_remove(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))

        db.execute("remove foo[0:6:2]")

        assert _length_matches(db, "foo", np.uint32) == (3, 3)

    def test_get_length_after_an_empty_remove_is_unchanged(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))

        db.execute("remove foo[6:]")

        assert _length_matches(db, "foo", np.uint32) == (6, 6)

    def test_get_length_after_a_destructive_read(self, db):
        """db.read of a REMOVE goes through numstore_remove_malloc, not
        numstore_remove - a separate copy of the same size update."""
        db.execute("create foo f64")
        db.write("insert foo 0 5", np.arange(5, dtype=np.float64))

        np.testing.assert_array_equal(db.read("remove foo[0:2]"), [0.0, 1.0])

        assert _length_matches(db, "foo", np.float64) == (3, 3)

    @pytest.mark.parametrize(
        "type_str,dtype", [("u8", np.uint8), ("u32", np.uint32), ("f64", np.float64)]
    )
    def test_get_length_after_a_remove_is_independent_of_element_size(
        self, db, type_str, dtype
    ):
        db.execute(f"create foo {type_str}")
        db.write("insert foo 0 10", np.arange(10, dtype=dtype))

        db.execute("remove foo[0:4]")

        assert _length_matches(db, "foo", dtype) == (6, 6)

    def test_repeated_removes_keep_the_length_consistent(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 10", np.arange(10, dtype=np.uint32))

        for expected in (8, 6, 4, 2, 0):
            db.execute("remove foo[0:2]")
            assert db.get("get foo").length == expected

    def test_var_shape_after_a_remove(self, db):
        db.execute("create grid [3] u32")
        db.write("insert grid 0 4", np.arange(12, dtype=np.uint32).reshape(4, 3))

        db.execute("remove grid[0:2]")

        assert db.get("get grid").shape == (2, 3)
        assert db.read("read grid").shape == (2, 3)

    def test_insert_after_a_remove_lands_at_the_right_offset(self, db):
        """A wrong stored length makes the next offset resolve against it."""
        db.execute("create foo u32")
        db.write("insert foo 0 6", np.arange(6, dtype=np.uint32))
        db.execute("remove foo[0:3]")

        db.write("insert foo 3 2", np.array([70, 80], dtype=np.uint32))

        np.testing.assert_array_equal(db.read("read foo"), [3, 4, 5, 70, 80])
        assert db.get("get foo").length == 5

    def test_get_length_after_a_remove_survives_reopen(self, db_dir):
        db = ns.Database("r.db")
        db.execute("create foo u32")
        db.write("insert foo 0 10", np.arange(10, dtype=np.uint32))
        db.execute("remove foo[0:4]")
        assert db.get("get foo").length == 6
        db.close()

        db = ns.Database("r.db")
        try:
            assert db.get("get foo").length == 6
            np.testing.assert_array_equal(db.read("read foo"), [4, 5, 6, 7, 8, 9])
        finally:
            db.close()

    ############# Stride semantics shared by READ, WRITE and REMOVE
    #
    # A bracketed range without a colon is a bare index and covers exactly one
    # element. `stride_resolve` used to ignore the parser's COLON_PRESENT flag, so a
    # missing stop always meant "to the end" and `foo[2]` silently behaved like
    # `foo[2:]`. The tests below pin the distinction down for every operation that
    # takes a range.

    # A bare index covers exactly one element
    @pytest.mark.parametrize("index,expected", [(0, 0), (2, 2), (5, 5)])
    def test_read_a_bare_index_is_one_element(self, foo, index, expected):
        np.testing.assert_array_equal(foo.read(f"read foo[{index}]"), [expected])

    def test_read_a_bare_negative_index_is_one_element(self, foo):
        np.testing.assert_array_equal(foo.read("read foo[-1]"), [5])
        np.testing.assert_array_equal(foo.read("read foo[-6]"), [0])

    def test_a_bare_index_is_not_an_open_range(self, foo):
        """The regression itself: [2] and [2:] must not agree."""
        bare = foo.read("read foo[2]")
        open_ended = foo.read("read foo[2:]")

        assert bare.size == 1
        assert open_ended.size == 4
        np.testing.assert_array_equal(bare, open_ended[:1])

    def test_a_bare_index_past_the_end_is_an_empty_range(self, foo):
        assert foo.read_into("read foo[6]", np.zeros(6, dtype=np.uint32)) == 0
        assert foo.read_into("read foo[99]", np.zeros(6, dtype=np.uint32)) == 0

    def test_a_bare_index_on_a_strict_array_is_one_element(self, db):
        db.execute("create grid [3] u32")
        db.write("insert grid 0 3", np.arange(9, dtype=np.uint32).reshape(3, 3))

        # one row, not one scalar element - grid's element type is itself an array
        assert db.read("read grid[1]").shape == (1, 3)
        np.testing.assert_array_equal(db.read("read grid[1]"), [[3, 4, 5]])
        assert db.read("read grid[1:]").shape == (2, 3)

    # The same rule for WRITE
    def test_write_to_a_bare_index_touches_one_element(self, foo):
        assert foo.write("write foo[2]", [70, 80, 90]) == 1
        np.testing.assert_array_equal(foo.read("read foo"), [0, 1, 70, 3, 4, 5])

    def test_write_to_an_open_range_touches_the_rest(self, foo):
        assert foo.write("write foo[2:]", [70, 80, 90]) == 3
        np.testing.assert_array_equal(foo.read("read foo"), [0, 1, 70, 80, 90, 5])

    def test_write_to_a_bare_index_past_the_end_is_a_noop(self, foo):
        assert foo.write("write foo[6]", [70]) == 0
        np.testing.assert_array_equal(foo.read("read foo"), np.arange(6))

    # And for REMOVE
    def test_remove_a_bare_index_drops_one_element(self, foo):
        foo.execute("remove foo[2]")

        np.testing.assert_array_equal(foo.read("read foo"), [0, 1, 3, 4, 5])
        assert foo.get("get foo").length == 5

    def test_remove_an_open_range_drops_the_rest(self, foo):
        foo.execute("remove foo[2:]")

        np.testing.assert_array_equal(foo.read("read foo"), [0, 1])
        assert foo.get("get foo").length == 2

    def test_destructive_read_of_a_bare_index_returns_one_element(self, foo):
        np.testing.assert_array_equal(foo.read("remove foo[2]"), [2])
        np.testing.assert_array_equal(foo.read("read foo"), [0, 1, 3, 4, 5])

    # Colon forms keep their old meaning
    def test_an_explicit_single_element_range_still_works(self, foo):
        np.testing.assert_array_equal(foo.read("read foo[2:3]"), [2])

    @pytest.mark.parametrize("rng", ["", "[:]", "[0:]", "[0::1]"])
    def test_whole_array_forms_agree(self, foo, rng):
        np.testing.assert_array_equal(foo.read(f"read foo{rng}"), np.arange(6))

    # Ranges clamp rather than raise
    @pytest.mark.parametrize("rng", ["[0:99]", "[-99:]", "[-99:99]"])
    def test_an_oversized_range_clamps_to_the_array(self, foo, rng):
        np.testing.assert_array_equal(foo.read(f"read foo{rng}"), np.arange(6))

    def test_a_start_past_the_end_is_an_empty_range(self, foo):
        assert foo.read_into("read foo[7:]", np.zeros(6, dtype=np.uint32)) == 0

    # Steps
    def test_a_step_spreads_the_range_out(self, foo):
        np.testing.assert_array_equal(foo.read("read foo[0:6:2]"), [0, 2, 4])

    @pytest.mark.parametrize("step", [0, -1, -2])
    def test_a_non_positive_step_is_rejected(self, foo, step):
        with pytest.raises(RuntimeError, match="stride step must be positive"):
            foo.read(f"read foo[0:6:{step}]")

    ############# get_all
    def test_get_all_on_an_empty_database(self, db):
        assert db.get_all() == []

    def test_get_all_returns_every_variable(self, db):
        db.execute("create a u8")
        db.execute("create b [4]f32")
        db.execute("create c struct { x u32, y f64 }")

        # No ordering is promised, so compare by name
        by_name = {v.name: v for v in db.get_all()}
        assert set(by_name) == {"a", "b", "c"}
        assert by_name["b"] == ns.Var("b", "[4]f32", 0, 16)

    def test_get_all_matches_get(self, db):
        db.execute("create foo u32")
        db.execute("create bar [2]f64")

        by_name = {v.name: v for v in db.get_all()}
        assert by_name["foo"] == db.get("get foo")
        assert by_name["bar"] == db.get("get bar")

    def test_get_all_tracks_length(self, db):
        db.execute("create foo u32")
        src = np.arange(5, dtype=np.uint32)
        db.write(f"insert foo 0 {src.size}", src)

        (var,) = db.get_all()
        assert var.length == src.size
        assert var.shape == (5,)
        assert var.dtype == np.dtype(np.uint32)

    def test_get_all_holds_no_c_handle(self, db):
        db.execute("create foo u32")

        (var,) = db.get_all()
        assert isinstance(var, ns.Var)
        assert {f.name for f in dataclasses.fields(var)} == {
            "name",
            "type",
            "length",
            "tsize",
        }


class TestQuerySyntaxEdges:
    """Corners of the query grammar, written to Python's slicing intuition.

    The xfails here are the interesting part: each one is a place where the
    query language reads like Python but doesn't behave like it.
    """

    ############# Omitted slice bounds
    @pytest.mark.parametrize(
        "rng,expected",
        [
            pytest.param("[:3]", [0, 1, 2], id="no-start"),
            pytest.param("[3:]", [3, 4, 5], id="no-stop"),
            pytest.param("[:]", [0, 1, 2, 3, 4, 5], id="neither"),
            pytest.param("[::2]", [0, 2, 4], id="step-only"),
            pytest.param("[1::2]", [1, 3, 5], id="start-and-step"),
            pytest.param("[:4:2]", [0, 2], id="stop-and-step"),
        ],
    )
    def test_omitted_bounds_match_python(self, foo, rng, expected):
        np.testing.assert_array_equal(foo.read(f"read foo{rng}"), expected)

    ############# Negative indices
    @pytest.mark.parametrize(
        "rng,expected",
        [
            pytest.param("[-3:]", [3, 4, 5], id="last-three"),
            pytest.param("[-6:]", [0, 1, 2, 3, 4, 5], id="all-of-it"),
            pytest.param("[-3:-1]", [3, 4], id="both-negative"),
            pytest.param("[1:-1]", [1, 2, 3, 4], id="mixed"),
            pytest.param("[-5:-1:2]", [1, 3], id="negative-with-step"),
        ],
    )
    def test_negative_ranges_match_python(self, foo, rng, expected):
        np.testing.assert_array_equal(foo.read(f"read foo{rng}"), expected)

    def test_a_negative_insert_offset_counts_from_the_end(self, foo):
        """-1 should mean "before the last element", the way foo[-1] does."""
        foo.write("insert foo -1 1", np.array([99], dtype=np.uint32))

        np.testing.assert_array_equal(foo.read("read foo"), [0, 1, 2, 3, 4, 99, 5])

    def test_remove_a_negative_range(self, foo):
        foo.execute("remove foo[-2:]")

        np.testing.assert_array_equal(foo.read("read foo"), [0, 1, 2, 3])

    ############# Strided writes
    def test_a_strided_write_touches_every_nth_element(self, foo):
        assert foo.write("write foo[0:6:2]", [70, 80, 90]) == 3

        np.testing.assert_array_equal(foo.read("read foo"), [70, 1, 80, 3, 90, 5])

    def test_a_strided_write_is_bounded_by_the_data(self, foo):
        assert foo.write("write foo[0:6:2]", [70]) == 1

        np.testing.assert_array_equal(foo.read("read foo"), [70, 1, 2, 3, 4, 5])

    def test_a_negative_bare_index_write_hits_the_last_element(self, foo):
        assert foo.write("write foo[-1]", [99]) == 1

        np.testing.assert_array_equal(foo.read("read foo"), [0, 1, 2, 3, 4, 99])

    ############# Whitespace and casing
    @pytest.mark.parametrize(
        "query",
        [
            pytest.param("create   foo    u32", id="extra-spaces"),
            pytest.param("create\nfoo\nu32", id="newlines"),
            pytest.param("\tcreate foo u32\t", id="tabs"),
            pytest.param("create foo u32   ", id="trailing-space"),
        ],
    )
    def test_whitespace_is_insignificant(self, db, query):
        db.execute(query)

        assert db.get("get foo") == ns.Var("foo", "u32", 0, 4)

    def test_whitespace_inside_a_slice(self, foo):
        np.testing.assert_array_equal(foo.read("read foo[ 1 : 3 ]"), [1, 2])

    @pytest.mark.xfail(
        reason="verbs are matched case-sensitively: check_keyword in ns_lexer.c "
        "strncmp's against the lowercase spelling, so CREATE lexes as an "
        "identifier and the parser reports 'Expected a valid operation at pos: 0'.",
        strict=True,
        raises=RuntimeError,
    )
    def test_verbs_are_case_insensitive(self, db):
        db.execute("CREATE foo u32")

        assert db.get("GET foo").name == "foo"

    def test_type_codes_are_case_sensitive(self, db):
        # u32 is a type; U32 is not
        with pytest.raises(RuntimeError):
            db.execute("create foo U32")

    def test_a_trailing_semicolon_is_accepted(self, db):
        db.execute("create foo u32;")

        assert db.get("get foo").name == "foo"

    @pytest.mark.xfail(
        reason="the parser stops once it has a complete statement and silently "
        "drops whatever follows, so typos in a suffix clause are never reported. "
        "This is the root cause of the blimit group below.",
        strict=True,
    )
    def test_trailing_junk_is_rejected(self, db):
        with pytest.raises(RuntimeError):
            db.execute("create foo u32 and then some")

    ############# blimit
    #
    # `blimit N` is documented (docs/index.md) and used in 14 places in this
    # suite, but it is not implemented: "blimit" is not a keyword in
    # check_keyword (csrc/nscore/compiler/ns_lexer.c), so `read foo[0:] blimit 8`
    # parses as `read foo[0:]` plus trailing junk that the parser drops. Even
    # `blimit notanumber` and `blimit -4` are accepted.
    #
    # Every existing test that passes a blimit still passes with it deleted --
    # the buffer length is what actually bounds read_into. These three pin the
    # clause down so that implementing it turns the xfails green.
    @pytest.mark.xfail(
        reason="blimit is not implemented; the clause is parsed as trailing "
        "junk and dropped, so the read is bounded only by the buffer.",
        strict=True,
    )
    def test_blimit_caps_the_read(self, foo):
        out = np.zeros(6, dtype=np.uint32)

        # room for two u32s, so two elements come back
        assert foo.read_into("read foo[0:] blimit 8", out) == 2
        np.testing.assert_array_equal(out[:2], [0, 1])

    @pytest.mark.xfail(
        reason="blimit is not implemented; see test_blimit_caps_the_read.",
        strict=True,
    )
    def test_a_blimit_that_is_not_a_multiple_of_the_element_size_truncates(self, foo):
        out = np.zeros(6, dtype=np.uint32)

        # 10 bytes is two whole u32s and half of a third
        assert foo.read_into("read foo[0:] blimit 10", out) == 2

    @pytest.mark.parametrize(
        "clause", ["blimit 1024", "blimit notanumber", "blimit -4"]
    )
    def test_a_blimit_never_widens_the_read_past_the_buffer(self, foo, clause):
        """Passes today only because the clause is ignored outright."""
        out = np.zeros(3, dtype=np.uint32)

        assert foo.read_into(f"read foo[0:] {clause}", out) == 3

    ############# Names
    def test_a_long_variable_name(self, db):
        name = "v" + "x" * 200

        db.execute(f"create {name} u32")

        assert db.get(f"get {name}").name == name

    def test_names_are_case_sensitive(self, db):
        db.execute("create foo u32")
        db.execute("create FOO f64")

        assert db.get("get foo").type == "u32"
        assert db.get("get FOO").type == "f64"


class TestDeleteVerb:
    """`delete [if exists] <name>` and `get if exists <name>`.

    Both are in the grammar (parse_query_delete / parse_query_get in
    csrc/nscore/compiler/parsers/ns_parse_query.c) and neither had a Python
    test before this file.
    """

    ############# delete
    def test_delete_removes_the_variable(self, db):
        db.execute("create foo u32")

        db.execute("delete foo")

        with pytest.raises(RuntimeError):
            db.get("get foo")

    def test_delete_removes_it_from_get_all(self, db):
        db.execute("create foo u32")
        db.execute("create bar f64")

        db.execute("delete foo")

        assert {v.name for v in db.get_all()} == {"bar"}

    def test_delete_discards_the_data(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 4", np.arange(4, dtype=np.uint32))

        db.execute("delete foo")
        db.execute("create foo u32")

        # a recreated variable starts empty, it does not inherit the old rows
        assert db.get("get foo").length == 0
        assert db.read("read foo").size == 0

    def test_delete_frees_the_name_for_a_different_type(self, db):
        db.execute("create foo u32")

        db.execute("delete foo")
        db.execute("create foo struct { a u8, b f64 }")

        assert db.get("get foo").type == "struct { a u8, b f64 }"

    def test_deleting_a_nonexistent_variable_raises(self, db):
        with pytest.raises(RuntimeError):
            db.execute("delete nope")

    def test_delete_of_a_large_variable(self, db):
        db.execute("create big u32")
        db.write("insert big 0 20000", np.arange(20000, dtype=np.uint32))

        db.execute("delete big")

        assert db.get_all() == []

    def test_delete_survives_a_reopen(self, db_dir):
        db = ns.Database("d.db")
        db.execute("create foo u32")
        db.execute("create bar u32")
        db.execute("delete foo")
        db.close()

        db = ns.Database("d.db")
        try:
            assert {v.name for v in db.get_all()} == {"bar"}
        finally:
            db.close()

    def test_a_rolled_back_delete_keeps_the_variable(self, db):
        db.execute("create foo u32")
        db.write("insert foo 0 3", np.arange(3, dtype=np.uint32))

        txn = db.begin()
        txn.execute("delete foo")
        txn.rollback()

        np.testing.assert_array_equal(db.read("read foo"), [0, 1, 2])

    def test_a_committed_delete_inside_a_transaction(self, db):
        db.execute("create foo u32")

        with db.begin() as txn:
            txn.execute("delete foo")

        assert db.get_all() == []

    ############# delete if exists
    @pytest.mark.xfail(
        reason="the `if exists` flag is dead. parse_query_delete stores it in "
        "query.delete.if_exists, ns_query.c compares it, and nothing else ever "
        "reads it -- so the executor still raises \"Variable: nope doesn't "
        'exist", which is the one thing the clause exists to prevent.',
        strict=True,
        raises=RuntimeError,
    )
    def test_delete_if_exists_on_a_missing_variable_is_a_noop(self, db):
        db.execute("delete if exists nope")

        assert db.get_all() == []

    def test_delete_if_exists_still_deletes_what_is_there(self, db):
        db.execute("create foo u32")

        db.execute("delete if exists foo")

        assert db.get_all() == []

    @pytest.mark.parametrize(
        "query",
        [
            pytest.param("delete if foo", id="if-without-exists"),
            pytest.param("delete exists foo", id="exists-without-if"),
            pytest.param("delete if exists", id="no-name"),
            pytest.param("delete if not exists foo", id="if-not-exists"),
        ],
    )
    def test_malformed_delete_raises(self, db, query):
        with pytest.raises(RuntimeError):
            db.execute(query)

    ############# get if exists
    def test_get_if_exists_finds_an_existing_variable(self, db):
        db.execute("create foo u32")

        assert db.get("get if exists foo") == ns.Var("foo", "u32", 0, 4)

    @pytest.mark.xfail(
        reason="same dead flag as delete: query.get.if_exists is parsed and "
        "stored but never read, so the miss raises instead of reporting "
        "absence. Whoever implements it also has to decide what the Python "
        "binding returns -- Var is not nullable today.",
        strict=True,
        raises=RuntimeError,
    )
    def test_get_if_exists_on_a_missing_variable_returns_none(self, db):
        """The point of `if exists` is to ask without it being an error."""
        assert db.get("get if exists nope") is None
