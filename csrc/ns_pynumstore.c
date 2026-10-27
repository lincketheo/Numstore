/// Copyright 2026 Theo Lincke
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.

// ns_pynumstore.h must come first: it includes <Python.h> (which has to precede
// every system header) and sets the NumPy API macros before numpy/arrayobject.h
// is pulled in. Including a numpy header before it locks in the wrong config.
#include "core/ns_error.h"
#include "nscore/algorithms/var/ns_var_algorithms.h"
#include "nscore/variables/ns_variables.h"
#define PYNUMSTORE_MODULE_MAIN
#include "core/ns_csx_assert.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "ns_pynumstore.h"
#include "nscore/compiler/ns_compiler.h"
#include "nscore/nsdb/ns_nsdb.h"
#include "nscore/types/ns_types.h"

//////////////////////////////// Constants

const char DB_CAPSULE[]   = "numstore.db";
const char TXN_CAPSULE[]  = "numstore.txn";
const char VAR_CAPSULE[]  = "numstore.var";
const char PLAN_CAPSULE[] = "numstore.plan";

char       TXN_CLOSED_SENTINEL;
char       DB_CLOSED_SENTINEL;
char       VAR_CLOSED_SENTINEL;
char       PLAN_CLOSED_SENTINEL;

//////////////////////////////// Error handling

static void
_pyns_set_error_from_e (PyObject *exc_type, error *e)
{
  ASSERT (e->cause_code < 0);
  // cause_msg is an inline buffer now, so an empty message - not a NULL one -
  // is what "no detail" looks like.
  PyErr_SetString (exc_type, (e->cmlen > 0) ? e->cause_msg : "numstore operation failed");
}

// Every nsdb call reports through the error the database holds a pointer to.
// Take the message and clear it, so the next call starts from a clean error.
static void
_pyns_set_error_from_nsdb (struct nsdb *ns)
{
  error *e = ns->e;
  PyErr_SetString (
      PyExc_RuntimeError,
      (e->cause_code < 0 && e->cmlen > 0) ? e->cause_msg : "numstore operation failed"
  );
  error_reset (e);
}

static void
_pyns_set_error_from_plan (struct nsdb_plan *plan)
{
  // A plan has no error of its own - it reports through the db it belongs to
  _pyns_set_error_from_nsdb (plan->parent);
}

//////////////////////////////// Database lifecycle
//
// nsdb takes the OS layer and the error buffer from whoever opens it, so this
// module owns both for as long as the database is open.

static struct nsdb *
_pyns_db_open (const char *path, error *e)
{
  struct i_mem mem = default_mem ();

  struct i_os  os;
  if (system_os_create (mem, &os, e)) {
    return NULL;
  }

  struct nsdb *db = nsdb_open (path, mem, os, e);
  if (db == NULL) {
    system_os_free (os);
    return NULL;
  }

  // A numstore database, not a smart file
  if (nsdb_init_numstore (db, e)) {
    nsdb_close (db, e);
    system_os_free (os);
    return NULL;
  }

  // The database only keeps a pointer to its error, so that error has to
  // outlive this call - _pyns_db_close frees it again
  error *persistent = mem.table->malloc (mem.self, 1, sizeof *persistent, e);
  if (persistent == NULL) {
    nsdb_close (db, e);
    system_os_free (os);
    return NULL;
  }
  *persistent = error_create ();

  nsdb_set_error (db, persistent);

  // Calls that are handed no transaction wrap themselves in one
  nsdb_allow_auto_txn (db);

  return db;
}

static err_t
_pyns_db_close (struct nsdb *db)
{
  // These all live inside db - copy them out before nsdb_close frees it
  const struct i_os  os  = db->os;
  const struct i_mem mem = db->mem;
  error             *e   = db->e;

  error_reset (e);
  const err_t ret = nsdb_close (db, NULL);
  mem.table->free (mem.self, e);
  system_os_free (os);
  return ret;
}

//////////////////////////////// Unwrap

static void *
_unwrap_capsule (PyObject *capsule, const char *name, const void *sentinel)
{
  // Ensure that the object is a capsule
  if (!PyCapsule_CheckExact (capsule)) {
    PyErr_Format (
        PyExc_TypeError,
        "expected %s capsule, got %.200s",
        name,
        Py_TYPE (capsule)->tp_name
    );
    return NULL;
  }

  // Check the name ourselves - PyCapsule_GetPointer would raise a confusing
  // ValueError for e.g. a txn passed where a db is expected
  if (!PyCapsule_IsValid (capsule, name)) {
    const char *got = PyCapsule_GetName (capsule);
    PyErr_Format (
        PyExc_TypeError,
        "expected %s capsule, got %.200s capsule",
        name,
        got ? got : "unnamed"
    );
    return NULL;
  }

  // Get the pointer from the capsule
  void *ptr = PyCapsule_GetPointer (capsule, name);
  if (ptr == NULL) {
    return NULL; // error already set by PyCapsule_GetPointer
  }

  // Check if the object is closed
  if (ptr == sentinel) {
    PyErr_Format (PyExc_RuntimeError, "%s is already closed", name);
    return NULL;
  }

  return ptr;
}

// Converters for PyArg_ParseTuple's "O&". They can also be called directly
// for METH_O functions. Return 1 on success, 0 with an exception set.

#define DEFINE_STRICT_X_CONVERTER(type, shorthand, capsule_name, sentinel)            \
  static int _##shorthand##_arg (PyObject *obj, void *out)                            \
  {                                                                                   \
    /* Don't allow None */                                                            \
    if (obj == Py_None) {                                                             \
      PyErr_SetString (PyExc_TypeError, "expected " #shorthand " capsule, got None"); \
      return 0;                                                                       \
    }                                                                                 \
    type *p = _unwrap_capsule (obj, capsule_name, &sentinel);                         \
    if (p == NULL) {                                                                  \
      return 0;                                                                       \
    }                                                                                 \
    *(type **)out = p;                                                                \
    return 1;                                                                         \
  }

#define DEFINE_NULLABLE_X_CONVERTER(type, shorthand, capsule_name, sentinel) \
  static int _##shorthand##_arg (PyObject *obj, void *out)                   \
  {                                                                          \
    /* Allow None */                                                         \
    if (obj == Py_None) {                                                    \
      *(type **)out = NULL;                                                  \
      return 1;                                                              \
    }                                                                        \
    type *p = _unwrap_capsule (obj, capsule_name, &sentinel);                \
    if (p == NULL) {                                                         \
      return 0;                                                              \
    }                                                                        \
    *(type **)out = p;                                                       \
    return 1;                                                                \
  }

DEFINE_STRICT_X_CONVERTER (struct nsdb, db, DB_CAPSULE, DB_CLOSED_SENTINEL)
DEFINE_STRICT_X_CONVERTER (struct nsdb_var, var, VAR_CAPSULE, VAR_CLOSED_SENTINEL)
DEFINE_NULLABLE_X_CONVERTER (struct txn, txn, TXN_CAPSULE, TXN_CLOSED_SENTINEL)

// A plan points at its parent db (plan->parent), so it is only usable while
// that db is open. Each plan capsule holds a strong reference to its db
// capsule in the capsule context - that keeps the db from being garbage
// collected first, and lets us tell when the user explicitly closed it.

// Returns 1 if the plan's db is still open, 0 if closed, -1 on error
static int
_plan_db_is_open (PyObject *plan_capsule)
{
  PyObject *db_obj = PyCapsule_GetContext (plan_capsule);
  if (db_obj == NULL) {
    return PyErr_Occurred () ? -1 : 1; // no context: only during creation, db is open
  }

  void *db = PyCapsule_GetPointer (db_obj, DB_CAPSULE);
  if (db == NULL) {
    return -1;
  }

  return db != &DB_CLOSED_SENTINEL;
}

static int
_plan_arg (PyObject *obj, void *out)
{
  if (obj == Py_None) {
    PyErr_SetString (PyExc_TypeError, "expected plan capsule, got None");
    return 0;
  }

  struct nsdb_plan *p = _unwrap_capsule (obj, PLAN_CAPSULE, &PLAN_CLOSED_SENTINEL);
  if (p == NULL) {
    return 0;
  }

  int open = _plan_db_is_open (obj);
  if (open < 0) {
    return 0;
  }
  if (!open) {
    PyErr_SetString (PyExc_RuntimeError, "the database this plan belongs to is closed");
    return 0;
  }

  *(struct nsdb_plan **)out = p;
  return 1;
}

//////////// Finalizers (capsule destructors)
//
// These run while the capsule is being deallocated, so there is no point
// setting the capsule's pointer to a sentinel here - nothing can see it again.
//
// Never pass the capsule to PyErr_WriteUnraisable here: its refcount is
// already 0, and WriteUnraisable takes and drops a reference to its argument,
// which deallocates the capsule again and re-enters this destructor until the
// stack overflows. Pass NULL.

static void
pyns_db_capsule_finalizer (PyObject *capsule)
{
  // Preserve any exception already in flight
  PyObject *type, *value, *tb;
  PyErr_Fetch (&type, &value, &tb);

  void *ptr = PyCapsule_GetPointer (capsule, DB_CAPSULE);
  ASSERT (ptr);

  // Sentinel means the user explicitly closed it - otherwise close it here
  if (ptr != &DB_CLOSED_SENTINEL && _pyns_db_close ((struct nsdb *)ptr) < 0) {
    PyErr_SetString (PyExc_RuntimeError, "failed to close nsdb during cleanup");
    PyErr_WriteUnraisable (NULL);
  }

  PyErr_Restore (type, value, tb);
}

static void
pyns_var_capsule_finalizer (PyObject *capsule)
{
  PyObject *type, *value, *tb;
  PyErr_Fetch (&type, &value, &tb);

  void *ptr = PyCapsule_GetPointer (capsule, VAR_CAPSULE);
  ASSERT (ptr);

  if (ptr != &VAR_CLOSED_SENTINEL) {
    nsdb_var_free ((struct nsdb_var *)ptr);
  }

  PyErr_Restore (type, value, tb);
}

static void
pyns_txn_capsule_finalizer (PyObject *capsule)
{
  PyObject *type, *value, *tb;
  PyErr_Fetch (&type, &value, &tb);

  void *ptr = PyCapsule_GetPointer (capsule, TXN_CAPSULE);
  ASSERT (ptr);

  // For now - you MUST close a transaction during runtime
  // In the future - I may be able to attach a db lifecycle to
  // a txn but for now - it's a requirement
  if (ptr != &TXN_CLOSED_SENTINEL) {
    PyErr_SetString (PyExc_RuntimeError, "transaction was never committed or rolled back");
    PyErr_WriteUnraisable (NULL);
  }

  PyErr_Restore (type, value, tb);
}

static void
pyns_plan_capsule_finalizer (PyObject *capsule)
{
  PyObject *type, *value, *tb;
  PyErr_Fetch (&type, &value, &tb);

  void *ptr = PyCapsule_GetPointer (capsule, PLAN_CAPSULE);
  ASSERT (ptr);

  // Because we hold a reference to the db capsule, the db can't have been
  // garbage collected yet - it can only have been closed explicitly. If it's
  // still open, freeing the plan here is safe. If it was closed, the plan's
  // memory went with it.
  if (ptr != &PLAN_CLOSED_SENTINEL) {
    int open = _plan_db_is_open (capsule);
    if (open < 0) {
      PyErr_WriteUnraisable (NULL);
    } else if (open) {
      nsdb_plan_free ((struct nsdb_plan *)ptr);
    }
  }

  // Drop our reference to the db last - this may run the db finalizer
  Py_XDECREF (PyCapsule_GetContext (capsule));

  PyErr_Restore (type, value, tb);
}

//////////// Constructors
//
// These never take ownership on failure: if they return NULL, the caller
// still owns the handle and must free it.

static PyObject *
pyns_db_capsule_new (struct nsdb *db)
{
  ASSERT (db);
  ASSERT ((void *)db != (void *)&DB_CLOSED_SENTINEL);
  return PyCapsule_New (db, DB_CAPSULE, pyns_db_capsule_finalizer);
}

static PyObject *
pyns_txn_capsule_new (struct txn *txn)
{
  ASSERT (txn);
  ASSERT ((void *)txn != (void *)&TXN_CLOSED_SENTINEL);
  return PyCapsule_New (txn, TXN_CAPSULE, pyns_txn_capsule_finalizer);
}

static PyObject *
pyns_plan_capsule_new (struct nsdb_plan *plan)
{
  ASSERT (plan);
  ASSERT ((void *)plan != (void *)&PLAN_CLOSED_SENTINEL);
  return PyCapsule_New (plan, PLAN_CAPSULE, pyns_plan_capsule_finalizer);
}

static PyObject *
pyns_var_capsule_new (struct nsdb_var *var)
{
  ASSERT (var);
  ASSERT ((void *)var != (void *)&VAR_CLOSED_SENTINEL);
  return PyCapsule_New (var, VAR_CAPSULE, pyns_var_capsule_finalizer);
}

//////////////////////////////// Elsize

static Py_ssize_t
elsize (PyArray_Descr *type)
{
#if NPY_FEATURE_VERSION >= NPY_2_0_API_VERSION
  return (type)->elsize;
#else
  return PyDataType_ELSIZE (type);
#endif
}

//////////////////////////////// Dims

struct dims_vec
{
  npy_intp *data;
  int       len;
  int       cap;
};

static void
dims_vec_create (struct dims_vec *v)
{
  v->data = NULL;
  v->len  = 0;
  v->cap  = 0;
}

static int
dims_vec_append (struct dims_vec *v, npy_intp dim)
{
  if (v->len == v->cap) {
    int       new_cap = (v->cap == 0) ? 4 : v->cap * 2;

    npy_intp *tmp     = PyMem_Realloc (v->data, (size_t)new_cap * sizeof (npy_intp));
    if (tmp == NULL) {
      PyErr_NoMemory ();
      return -1;
    }

    v->data = tmp;
    v->cap  = new_cap;
  }

  v->data[v->len++] = dim;
  return 0;
}

static int
dims_vec_append_many (struct dims_vec *v, const u32 *dims, u32 count)
{
  for (u32 i = 0; i < count; i++) {
    if (dims_vec_append (v, (npy_intp)dims[i]) != 0) {
      return -1;
    }
  }
  return 0;
}

static void
dims_vec_free (struct dims_vec *v)
{
  PyMem_Free (v->data);
  v->data = NULL;
  v->len  = 0;
  v->cap  = 0;
}

//////////////////////////////// Type conversion
//
// Every builder below checks each allocation before making the next Python
// API call - calling into the API with an exception already set is undefined.

static PyArray_Descr *pyns_type_to_dtype (const struct type *t);

// Build a complex valued struct: [("re", comp), ("im", comp)]
static PyArray_Descr *
build_complex_struct (int component_typenum)
{
  static const char *names[2] = {"re", "im"};

  PyArray_Descr     *comp     = NULL;
  PyObject          *fields   = NULL;
  PyArray_Descr     *out      = NULL;

  comp                        = PyArray_DescrFromType (component_typenum);
  if (comp == NULL) {
    goto theend;
  }

  fields = PyList_New (2);
  if (fields == NULL) {
    goto theend;
  }

  for (int i = 0; i < 2; i++) {
    // "O" takes a new reference to comp, so no manual INCREF needed
    PyObject *tup = Py_BuildValue ("(sO)", names[i], (PyObject *)comp);
    if (tup == NULL) {
      goto theend;
    }
    PyList_SET_ITEM (fields, i, tup); // steals tup
  }

  if (PyArray_DescrConverter (fields, &out) != NPY_SUCCEED) {
    out = NULL;
  }

theend:
  Py_XDECREF (comp);
  Py_XDECREF (fields);
  return out;
}

static PyArray_Descr *
primitive_to_dtype (enum prim_t p)
{
  int        typenum;

  // For long double types: numpy's longdouble is whatever the C compiler's
  // long double is - 16 bytes on x86-64 Linux, but only 8 on MSVC and on
  // Apple Silicon. Refuse rather than hand back a dtype of the wrong size.
  Py_ssize_t expect = 0;

  switch (p) {
    case U8: typenum = NPY_UINT8; break;
    case U16: typenum = NPY_UINT16; break;
    case U32: typenum = NPY_UINT32; break;
    case U64: typenum = NPY_UINT64; break;
    case I8: typenum = NPY_INT8; break;
    case I16: typenum = NPY_INT16; break;
    case I32: typenum = NPY_INT32; break;
    case I64: typenum = NPY_INT64; break;
    case F16: typenum = NPY_FLOAT16; break;
    case F32: typenum = NPY_FLOAT32; break;
    case F64: typenum = NPY_FLOAT64; break;
    case F128:
      typenum = NPY_LONGDOUBLE;
      expect  = 16;
      break;
    case CF64: typenum = NPY_COMPLEX64; break;
    case CF128: typenum = NPY_COMPLEX128; break;
    case CF256:
      typenum = NPY_CLONGDOUBLE;
      expect  = 32;
      break;

    // Complex types numpy doesn't have natively
    case CF32: return build_complex_struct (NPY_FLOAT16);
    case CI16: return build_complex_struct (NPY_INT8);
    case CI32: return build_complex_struct (NPY_INT16);
    case CI64: return build_complex_struct (NPY_INT32);
    case CI128: return build_complex_struct (NPY_INT64);
    case CU16: return build_complex_struct (NPY_UINT8);
    case CU32: return build_complex_struct (NPY_UINT16);
    case CU64: return build_complex_struct (NPY_UINT32);
    case CU128: return build_complex_struct (NPY_UINT64);

    default: {
      PyErr_Format (PyExc_ValueError, "unknown numstore primitive: %d", (int)p);
      return NULL;
    }
  }

  PyArray_Descr *d = PyArray_DescrFromType (typenum);
  if (d == NULL) {
    return NULL;
  }

  if (expect != 0 && elsize (d) != expect) {
    PyErr_Format (
        PyExc_NotImplementedError,
        "numstore type needs a %zd byte float, but numpy's long double is %zd "
        "bytes on this platform",
        expect,
        elsize (d)
    );
    Py_DECREF (d);
    return NULL;
  }

  return d;
}

static PyArray_Descr *
struct_to_dtype (const struct struct_t *st)
{
  ASSERT (st->len > 0);
  PyObject      *fields = NULL; // List of (name, sub) tuples
  PyObject      *name   = NULL; // name of each field
  PyArray_Descr *sub    = NULL; // sub type of each field
  PyArray_Descr *out    = NULL; // The result

  fields                = PyList_New (st->len);
  if (fields == NULL) {
    goto fail;
  }

  for (u16 i = 0; i < st->len; i++) {
    name = PyUnicode_FromStringAndSize (st->keys[i].data, (Py_ssize_t)st->keys[i].len);
    if (name == NULL) {
      goto fail;
    }

    sub = pyns_type_to_dtype (st->types[i]);
    if (sub == NULL) {
      goto fail;
    }

    // PyTuple_Pack takes its own references - it doesn't steal
    PyObject *tup = PyTuple_Pack (2, name, (PyObject *)sub);
    if (tup == NULL) {
      goto fail;
    }
    Py_CLEAR (name);
    Py_CLEAR (sub);

    PyList_SET_ITEM (fields, i, tup); // steals tup
  }

  if (PyArray_DescrConverter (fields, &out) != NPY_SUCCEED) {
    goto fail;
  }

  Py_DECREF (fields);
  return out;

fail:
  Py_XDECREF (name);
  Py_XDECREF (sub);
  Py_XDECREF (fields);
  return NULL;
}

static PyArray_Descr *
union_to_dtype (const struct union_t *un)
{
  ASSERT (un->len > 0);
  PyObject      *names   = NULL;
  PyObject      *formats = NULL;
  PyObject      *offsets = NULL;
  PyObject      *spec    = NULL;
  PyArray_Descr *out     = NULL;

  names                  = PyList_New (un->len);
  if (names == NULL) {
    goto theend;
  }
  formats = PyList_New (un->len);
  if (formats == NULL) {
    goto theend;
  }
  offsets = PyList_New (un->len);
  if (offsets == NULL) {
    goto theend;
  }

  // Every member of a union sits at offset 0, itemsize is the largest member
  Py_ssize_t max_size = 0;
  for (u16 i = 0; i < un->len; i++) {
    PyObject *name = PyUnicode_FromStringAndSize (un->keys[i].data, (Py_ssize_t)un->keys[i].len);
    if (name == NULL) {
      goto theend;
    }
    PyList_SET_ITEM (names, i, name);

    PyArray_Descr *sub = pyns_type_to_dtype (un->types[i]);
    if (sub == NULL) {
      goto theend;
    }
    PyList_SET_ITEM (formats, i, (PyObject *)sub);

    PyObject *off = PyLong_FromLong (0);
    if (off == NULL) {
      goto theend;
    }
    PyList_SET_ITEM (offsets, i, off);

    Py_ssize_t isize = elsize (sub);
    if (isize > max_size) {
      max_size = isize;
    }
  }

  // "O" takes new references, "n" is a Py_ssize_t
  spec = Py_BuildValue (
      "{s:O,s:O,s:O,s:n}",
      "names",
      names,
      "formats",
      formats,
      "offsets",
      offsets,
      "itemsize",
      max_size
  );
  if (spec == NULL) {
    goto theend;
  }

  if (PyArray_DescrConverter (spec, &out) != NPY_SUCCEED) {
    out = NULL;
  }

theend:
  // A list that failed half way through still holds NULL slots - list
  // dealloc handles those fine.
  Py_XDECREF (names);
  Py_XDECREF (formats);
  Py_XDECREF (offsets);
  Py_XDECREF (spec);
  return out;
}

static PyArray_Descr *
sarray_to_dtype (const struct sarray_t *sa)
{
  ASSERT (sa->rank > 0);
  PyArray_Descr *sub   = NULL;
  PyObject      *shape = NULL;
  PyObject      *spec  = NULL;
  PyArray_Descr *out   = NULL;

  sub                  = pyns_type_to_dtype (sa->t);
  if (sub == NULL) {
    goto theend;
  }

  shape = PyTuple_New (sa->rank);
  if (shape == NULL) {
    goto theend;
  }

  for (u16 i = 0; i < sa->rank; i++) {
    PyObject *d = PyLong_FromUnsignedLong ((unsigned long)sa->dims[i]);
    if (d == NULL) {
      goto theend;
    }
    PyTuple_SET_ITEM (shape, i, d); // steals d
  }

  // (base, shape) is numpy's subarray dtype spec
  spec = PyTuple_Pack (2, (PyObject *)sub, shape);
  if (spec == NULL) {
    goto theend;
  }

  if (PyArray_DescrConverter (spec, &out) != NPY_SUCCEED) {
    out = NULL;
  }

theend:
  Py_XDECREF (sub);
  Py_XDECREF (shape);
  Py_XDECREF (spec);
  return out;
}

static PyArray_Descr *
pyns_type_to_dtype (const struct type *t)
{
  ASSERT (t);

  switch (t->type) {
    case T_PRIM: return primitive_to_dtype (t->p);
    case T_STRUCT: return struct_to_dtype (&t->st);
    case T_UNION: return union_to_dtype (&t->un);
    case T_SARRAY: return sarray_to_dtype (&t->sa);
    default: {
      UNREACHABLE ();
    }
  }
}

static PyArray_Descr *
pyns_type_to_dtype_flatten_sarray (const struct type *t, b_size top, struct dims_vec *vec)
{
  ASSERT (t);

  // Top length is the first dimension of the shape
  // ensure it is wrapped
  if (top > (b_size)NPY_MAX_INTP) {
    PyErr_SetString (PyExc_OverflowError, "result has too many elements for numpy");
    return NULL;
  }

  if (dims_vec_append (vec, (npy_intp)top) != 0) {
    return NULL;
  }

  // Append each top level array
  const struct type *cur = t;
  while (cur->type == T_SARRAY) {
    const struct sarray_t *sa = &cur->sa;
    ASSERT (sa->rank > 0);

    if (dims_vec_append_many (vec, sa->dims, sa->rank) != 0) {
      return NULL;
    }

    cur = sa->t;
  }

  return pyns_type_to_dtype (cur);
}

static PyObject *
pyns_type_string (struct type *t)
{
  if (t == NULL) {
    PyErr_SetString (PyExc_RuntimeError, "variable has no type");
    return NULL;
  }

  // Ask for the buffer size first - a struct/union type has no bound worth
  // hard coding here.
  i32 needed = type_snprintf (NULL, 0, t);
  if (needed < 0) {
    PyErr_SetString (PyExc_RuntimeError, "failed to measure variable type");
    return NULL;
  }

  char *buf = malloc ((size_t)needed + 1);
  if (buf == NULL) {
    return PyErr_NoMemory ();
  }

  i32 len = type_snprintf (buf, (u32)needed + 1, t);
  if (len < 0) {
    free (buf);
    PyErr_SetString (PyExc_RuntimeError, "failed to render variable type");
    return NULL;
  }

  PyObject *ret = PyUnicode_FromStringAndSize (buf, (Py_ssize_t)len);
  free (buf);
  return ret;
}

PyObject *
pyns_ns_to_np (PyObject *Py_UNUSED (m), PyObject *arg)
{
  ALLOC_INIT (alloc);
  struct type    t;
  error          e   = error_create ();
  PyArray_Descr *ret = NULL;

  // Extract utf8 string from argument
  const char    *src = PyUnicode_AsUTF8 (arg);
  if (!src) {
    goto theend;
  }

  // compile the type string - a type that won't compile is a bad argument
  // value, not a database failure
  if (compile_type (&t, src, &alloc, &e)) {
    _pyns_set_error_from_e (PyExc_ValueError, &e);
    goto theend;
  }

  // Convert it to a numpy type
  ret = pyns_type_to_dtype (&t);

theend:
  ALLOC_CLOSE (alloc);
  return (PyObject *)ret;
}

PyObject *
pyns_ns_to_np_flatten (PyObject *Py_UNUSED (m), PyObject *args)
{
  const char *src;
  long long   n;

  // pyns_ns_to_np_flatten(type: str, n: int) -> (tuple[int, ...], np.dtype)
  if (!PyArg_ParseTuple (args, "sL", &src, &n)) {
    return NULL;
  }

  if (n < 0) {
    PyErr_SetString (PyExc_ValueError, "n must be >= 0");
    return NULL;
  }

  ALLOC_INIT (alloc);
  struct type     t;
  error           e = error_create ();
  struct dims_vec vec;
  PyArray_Descr  *descr = NULL;
  PyObject       *shape = NULL;
  PyObject       *ret   = NULL;

  dims_vec_create (&vec);

  // A type that won't compile is a bad argument value
  if (compile_type (&t, src, &alloc, &e)) {
    _pyns_set_error_from_e (PyExc_ValueError, &e);
    goto theend;
  }

  // Peel leading sarray dims into vec: vec = [n, dims...], descr = base type
  descr = pyns_type_to_dtype_flatten_sarray (&t, (b_size)n, &vec);
  if (descr == NULL) {
    goto theend;
  }

  shape = PyArray_IntTupleFromIntp (vec.len, vec.data);
  if (shape == NULL) {
    goto theend;
  }

  // PyTuple_Pack takes its own references - descr and shape are released below
  ret = PyTuple_Pack (2, shape, (PyObject *)descr);

theend:
  Py_XDECREF (descr);
  Py_XDECREF (shape);
  dims_vec_free (&vec);
  ALLOC_CLOSE (alloc);
  return ret;
}

//////////////////////////////// Lifecycle

PyObject *
pyns_open (PyObject *Py_UNUSED (m), PyObject *arg)
{
  // Check that the object is a string
  if (!PyUnicode_Check (arg)) {
    PyErr_SetString (PyExc_TypeError, "path must be str");
    return NULL;
  }

  // Get the path as a utf8 string
  const char *path = PyUnicode_AsUTF8 (arg);
  if (!path) {
    return NULL;
  }

  // Open the database. There is no database to report through yet, so a
  // failure here lands in a stack error
  error        e  = error_create ();
  struct nsdb *ns = _pyns_db_open (path, &e);
  if (ns == NULL) {
    _pyns_set_error_from_e (PyExc_RuntimeError, &e);
    return NULL;
  }

  // Wrap it in a capsule - close the db if that fails so it doesn't leak
  PyObject *ret = pyns_db_capsule_new (ns);
  if (ret == NULL) {
    _pyns_db_close (ns);
  }

  return ret;
}

PyObject *
pyns_close (PyObject *Py_UNUSED (m), PyObject *arg)
{
  // Unwrap the database capsule
  struct nsdb *ns;
  if (!_db_arg (arg, &ns)) {
    return NULL;
  }

  // Manually close it
  const err_t ret = _pyns_db_close (ns);

  // Mark closed even if close failed - the handle is unusable either way
  if (PyCapsule_SetPointer (arg, &DB_CLOSED_SENTINEL) < 0) {
    return NULL;
  }

  // Check close return status
  if (ret < 0) {
    PyErr_SetString (PyExc_RuntimeError, "Failed to close numstore database");
    return NULL;
  }

  Py_RETURN_NONE;
}

//////////////////////////////// Transactions

PyObject *
pyns_begin (PyObject *Py_UNUSED (m), PyObject *arg)
{
  // Get the wrapped database
  struct nsdb *ns;
  if (!_db_arg (arg, &ns)) {
    return NULL;
  }

  // Begin transaction
  error_reset (ns->e);
  struct txn *txn = nsdb_begin (ns, NULL);
  if (txn == NULL) {
    _pyns_set_error_from_nsdb (ns);
    return NULL;
  }

  // Wrap the transaction in a capsule - roll it back if that fails,
  // otherwise it stays open forever
  PyObject *ret = pyns_txn_capsule_new (txn);
  if (ret == NULL) {
    nsdb_rollback (ns, txn, NULL);
  }

  return ret;
}

PyObject *
pyns_commit (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb *db;
  struct txn  *txn;

  // pyns_commit(db: capsule, txn: capsule)
  if (!PyArg_ParseTuple (args, "O&O&", _db_arg, &db, _txn_arg, &txn)) {
    return NULL;
  }

  if (txn == NULL) {
    PyErr_SetString (PyExc_TypeError, "Nothing to commit - txn was None");
    return NULL;
  }

  PyObject *txn_obj = PyTuple_GET_ITEM (args, 1);

  // Do commit
  error_reset (db->e);
  if (nsdb_commit (db, txn, NULL) < 0) {
    _pyns_set_error_from_nsdb (db);
    // The txn is finished either way - don't try to rollback
    PyCapsule_SetPointer (txn_obj, &TXN_CLOSED_SENTINEL);
    return NULL;
  }

  // txn is now closed
  if (PyCapsule_SetPointer (txn_obj, &TXN_CLOSED_SENTINEL) < 0) {
    return NULL;
  }

  Py_RETURN_NONE;
}

PyObject *
pyns_rollback (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb *db;
  struct txn  *txn;

  // pyns_rollback(db: capsule, txn: capsule)
  if (!PyArg_ParseTuple (args, "O&O&", _db_arg, &db, _txn_arg, &txn)) {
    return NULL;
  }

  if (txn == NULL) {
    PyErr_SetString (PyExc_TypeError, "Nothing to roll back - txn was None");
    return NULL;
  }

  PyObject *txn_obj = PyTuple_GET_ITEM (args, 1);

  // Do rollback
  error_reset (db->e);
  if (nsdb_rollback (db, txn, NULL) < 0) {
    _pyns_set_error_from_nsdb (db);
    // The txn is finished either way
    PyCapsule_SetPointer (txn_obj, &TXN_CLOSED_SENTINEL);
    return NULL;
  }

  // txn is now closed
  if (PyCapsule_SetPointer (txn_obj, &TXN_CLOSED_SENTINEL) < 0) {
    return NULL;
  }

  Py_RETURN_NONE;
}

//////////////////////////////// Global operations

struct pyns_visit_vars_ctx
{
  PyObject    *list;
  struct nsdb *db;
};

static err_t
pyns_visit_var_consumer (struct variable *_var, void *_ctx, error *e)
{
  struct pyns_visit_vars_ctx *ctx = _ctx;

  // Allocate a variable
  struct nsdb_var            *var = nsdb_var_create (ctx->db->mem, e);
  if (var == NULL) {
    return error_trace (e);
  }

  // Variable is temporary in visitor - copy it to result variable
  if (var_copy (nsdb_var_var (var), _var, nsdb_var_alloc (var), e) < 0) {
    nsdb_var_free (var);
    return error_trace (e);
  }

  // Wrap in a capsule - capsule now owns var
  PyObject *next = pyns_var_capsule_new (var);
  if (next == NULL) {
    nsdb_var_free (var);
    return error_causef (e, ERR_NOMEM, "Failed to wrap a variable in a capsule");
  }

  // List takes its own reference; drop ours either way
  int rc = PyList_Append (ctx->list, next);
  Py_DECREF (next);
  if (rc < 0) {
    return error_causef (e, ERR_NOMEM, "Failed to append item to python list");
  }

  return SUCCESS;
}

PyObject *
pyns_get_all_vars (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb *db;
  struct txn  *txn;

  // pyns_get_all_vars(db: capsule, txn: capsule | None)
  if (!PyArg_ParseTuple (args, "O&O&", _db_arg, &db, _txn_arg, &txn)) {
    return NULL;
  }

  // Read for now - wait until lock table is better
  (void)txn;

  // Manually run this - no wrapper is available
  error_reset (db->e);

  PyObject *list = PyList_New (0);
  if (list == NULL) {
    return NULL; // MemoryError already set by Python
  }

  struct pyns_visit_vars_ctx ctx = {
      .db   = db,
      .list = list,
  };

  if (ns_visit_variables (db->p, pyns_visit_var_consumer, &ctx, db->e) < 0) {
    Py_DECREF (list); // frees list + every capsule (and its var) in it

    // The consumer fails on Python errors too - keep that exception
    // (MemoryError, say) rather than papering over it with the db's message.
    if (PyErr_Occurred ()) {
      error_reset (db->e);
    } else {
      _pyns_set_error_from_nsdb (db);
    }
    return NULL;
  }

  return list;
}

//////////////////////////////// Plans

PyObject *
pyns_plan_create (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb *db;
  const char  *query;

  // pyns_plan_create(db: capsule, query: str)
  if (!PyArg_ParseTuple (args, "O&s", _db_arg, &db, &query)) {
    return NULL;
  }

  PyObject         *db_obj = PyTuple_GET_ITEM (args, 0);

  // Create the plan - nsdb_plan_create takes the query literally, so it is
  // never read as a format string
  struct nsdb_plan *plan   = nsdb_plan_create (db, query, NULL);
  if (plan == NULL) {
    // There's no plan to ask for an error, so it comes from the db
    _pyns_set_error_from_nsdb (db);
    return NULL;
  }

  // Wrap it in a capsule
  PyObject *ret = pyns_plan_capsule_new (plan);
  if (ret == NULL) {
    nsdb_plan_free (plan);
    return NULL;
  }

  // Tie the plan to its db (see _plan_arg). If this fails, the DECREF runs
  // the finalizer, which frees the plan - context is still NULL there.
  Py_INCREF (db_obj);
  if (PyCapsule_SetContext (ret, db_obj) < 0) {
    Py_DECREF (db_obj);
    Py_DECREF (ret);
    return NULL;
  }

  return ret;
}

PyObject *
pyns_plan_close (PyObject *Py_UNUSED (m), PyObject *arg)
{
  // pyns_plan_close(plan: capsule)
  //
  // Doesn't use _plan_arg: closing a plan whose db is already closed is
  // allowed - there's just nothing left to free.
  struct nsdb_plan *plan = _unwrap_capsule (arg, PLAN_CAPSULE, &PLAN_CLOSED_SENTINEL);
  if (plan == NULL) {
    return NULL;
  }

  int open = _plan_db_is_open (arg);
  if (open < 0) {
    return NULL;
  }

  if (open) {
    nsdb_plan_free (plan);
  }

  if (PyCapsule_SetPointer (arg, &PLAN_CLOSED_SENTINEL) < 0) {
    return NULL;
  }

  Py_RETURN_NONE;
}

PyObject *
pyns_plan_execute (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb_plan *plan;
  struct txn       *txn;

  // pyns_plan_execute(plan: capsule, txn: capsule | None)
  if (!PyArg_ParseTuple (args, "O&O&", _plan_arg, &plan, _txn_arg, &txn)) {
    return NULL;
  }

  const sb_size ret = nsdb_plan_execute (plan, txn, NULL);
  if (ret < 0) {
    _pyns_set_error_from_plan (plan);
    return NULL;
  }

  Py_RETURN_NONE;
}

PyObject *
pyns_plan_get_var (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb_plan *plan;
  struct txn       *txn;

  // pyns_plan_get_var(plan: capsule, txn: capsule | None)
  if (!PyArg_ParseTuple (args, "O&O&", _plan_arg, &plan, _txn_arg, &txn)) {
    return NULL;
  }

  // Run the plan
  struct nsdb_var *var = nsdb_plan_get_var (plan, txn, NULL);
  if (var == NULL) {
    _pyns_set_error_from_plan (plan);
    return NULL;
  }

  // Create a new variable capsule (the constructor doesn't free on failure)
  PyObject *ret = pyns_var_capsule_new (var);
  if (ret == NULL) {
    nsdb_var_free (var);
  }

  return ret;
}

PyObject *
pyns_plan_read (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb_plan *plan;
  struct txn       *txn;
  Py_buffer         data;

  // pyns_plan_read(plan: capsule, txn: capsule | None, dest: Buffer)
  // w* = writable and C-contiguous; rejects bytes and strided views
  if (!PyArg_ParseTuple (args, "O&O&w*", _plan_arg, &plan, _txn_arg, &txn, &data)) {
    return NULL;
  }

  sb_size ret = nsdb_plan_read (plan, txn, data.buf, (b_size)data.len, NULL);

  // Release on every path - a leaked export leaves the caller's object locked
  // (e.g. a bytearray can never be resized again)
  PyBuffer_Release (&data);

  if (ret < 0) {
    _pyns_set_error_from_plan (plan);
    return NULL;
  }

  return PyLong_FromLongLong ((long long)ret);
}

PyObject *
pyns_plan_malloc (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb_plan *plan;
  struct txn       *txn;

  // pyns_plan_malloc(plan: capsule, txn: capsule | None) -> bytes
  if (!PyArg_ParseTuple (args, "O&O&", _plan_arg, &plan, _txn_arg, &txn)) {
    return NULL;
  }

  b_size size = 0;
  void  *buf  = nsdb_plan_read_malloc (plan, txn, &size, NULL);
  if (buf == NULL) {
    _pyns_set_error_from_plan (plan);
    return NULL;
  }

  if (size > (b_size)PY_SSIZE_T_MAX) {
    free (buf);
    PyErr_SetString (PyExc_OverflowError, "result is too large for a Python bytes object");
    return NULL;
  }

  // COPY data over (this could be made later to do zero copy - this is the
  // first start). free() must match the allocator the db was opened with -
  // default_mem() is the system one.
  PyObject *out = PyBytes_FromStringAndSize (buf, (Py_ssize_t)size);
  free (buf);

  return out; // NULL with MemoryError set if the copy failed
}

PyObject *
pyns_plan_write (PyObject *Py_UNUSED (m), PyObject *args)
{
  struct nsdb_plan *plan;
  struct txn       *txn;
  Py_buffer         data;

  // pyns_plan_write(plan: capsule, txn: capsule | None, src: Buffer)
  // y* = read-only is fine (it's a source), still requires C-contiguous
  if (!PyArg_ParseTuple (args, "O&O&y*", _plan_arg, &plan, _txn_arg, &txn, &data)) {
    return NULL;
  }

  sb_size ret = nsdb_plan_write (plan, txn, data.buf, (b_size)data.len, NULL);

  PyBuffer_Release (&data);

  if (ret < 0) {
    _pyns_set_error_from_plan (plan);
    return NULL;
  }

  return PyLong_FromLongLong ((long long)ret);
}

//////////////////////////////// Variables

PyObject *
pyns_var_free (PyObject *Py_UNUSED (m), PyObject *arg)
{
  struct nsdb_var *var;
  if (!_var_arg (arg, &var)) {
    return NULL;
  }

  // Same free function as the finalizer
  nsdb_var_free (var);

  // Without this, the finalizer frees it a second time
  if (PyCapsule_SetPointer (arg, &VAR_CLOSED_SENTINEL) < 0) {
    return NULL;
  }

  Py_RETURN_NONE;
}

PyObject *
pyns_var_name (PyObject *Py_UNUSED (m), PyObject *arg)
{
  struct nsdb_var *var;
  if (!_var_arg (arg, &var)) {
    return NULL;
  }

  struct string name = nsdb_var_name (var);
  return PyUnicode_FromStringAndSize (name.data, (Py_ssize_t)name.len);
}

PyObject *
pyns_var_length (PyObject *Py_UNUSED (m), PyObject *arg)
{
  struct nsdb_var *var;
  if (!_var_arg (arg, &var)) {
    return NULL;
  }

  return PyLong_FromUnsignedLongLong ((unsigned long long)nsdb_var_len (var));
}

PyObject *
pyns_var_tsize (PyObject *Py_UNUSED (m), PyObject *arg)
{
  struct nsdb_var *var;
  if (!_var_arg (arg, &var)) {
    return NULL;
  }

  struct type *t = nsdb_var_type (var);
  if (t == NULL) {
    PyErr_SetString (PyExc_RuntimeError, "variable has no type");
    return NULL;
  }

  return PyLong_FromUnsignedLongLong ((unsigned long long)type_byte_size (t));
}

PyObject *
pyns_var_type (PyObject *Py_UNUSED (m), PyObject *arg)
{
  struct nsdb_var *var;
  if (!_var_arg (arg, &var)) {
    return NULL;
  }

  return pyns_type_string (nsdb_var_type (var));
}

//////////////////////////////// Python Module
//
// Names match _pynumstore.pyi

static PyMethodDef pynumstore_methods[] = {
    // Utils
    {
        "_pyns_ns_to_np",
        pyns_ns_to_np,
        METH_O,
        "_pyns_ns_to_np(type: str) -> np.dtype",
    },
    {
        "_pyns_ns_to_np_flatten",
        pyns_ns_to_np_flatten,
        METH_VARARGS,
        "_pyns_ns_to_np_flatten(type: str, n: int) -> (shape, np.dtype)",
    },

    // Lifecycle
    {
        "_pyns_open",
        pyns_open,
        METH_O,
        "_pyns_open(path: str) -> db",
    },
    {
        "_pyns_close",
        pyns_close,
        METH_O,
        "_pyns_close(db) -> None",
    },

    // Transactions
    {
        "_pyns_begin",
        pyns_begin,
        METH_O,
        "_pyns_begin(db) -> txn",
    },
    {
        "_pyns_commit",
        pyns_commit,
        METH_VARARGS,
        "_pyns_commit(db, txn) -> None",
    },
    {
        "_pyns_rollback",
        pyns_rollback,
        METH_VARARGS,
        "_pyns_rollback(db, txn) -> None",
    },

    // Global
    {
        "_pyns_get_all_vars",
        pyns_get_all_vars,
        METH_VARARGS,
        "_pyns_get_all_vars(db, txn) -> List[var]",
    },

    // Plans
    {
        "_pyns_plan_create",
        pyns_plan_create,
        METH_VARARGS,
        "_pyns_plan_create(db, query: str) -> plan",
    },
    {
        "_pyns_plan_close",
        pyns_plan_close,
        METH_O,
        "_pyns_plan_close(plan) -> None",
    },
    {
        "_pyns_plan_execute",
        pyns_plan_execute,
        METH_VARARGS,
        "_pyns_plan_execute(plan, txn | None) -> None",
    },
    {
        "_pyns_plan_get_var",
        pyns_plan_get_var,
        METH_VARARGS,
        "_pyns_plan_get_var(plan, txn | None) -> var",
    },
    {
        "_pyns_plan_read",
        pyns_plan_read,
        METH_VARARGS,
        "_pyns_plan_read(plan, txn | None, dest: writable buffer) -> int",
    },
    {
        "_pyns_plan_malloc",
        pyns_plan_malloc,
        METH_VARARGS,
        "_pyns_plan_malloc(plan, txn | None) -> bytes",
    },
    {
        "_pyns_plan_write",
        pyns_plan_write,
        METH_VARARGS,
        "_pyns_plan_write(plan, txn | None, src: buffer) -> int",
    },

    // Variables
    {
        "_pyns_var_free",
        pyns_var_free,
        METH_O,
        "_pyns_var_free(var) -> None",
    },
    {
        "_pyns_var_name",
        pyns_var_name,
        METH_O,
        "_pyns_var_name(var) -> str",
    },
    {
        "_pyns_var_length",
        pyns_var_length,
        METH_O,
        "_pyns_var_length(var) -> int",
    },
    {
        "_pyns_var_tsize",
        pyns_var_tsize,
        METH_O,
        "_pyns_var_tsize(var) -> int",
    },
    {
        "_pyns_var_type",
        pyns_var_type,
        METH_O,
        "_pyns_var_type(var) -> str",
    },

    // End
    {NULL, NULL, 0, NULL},
};

static PyModuleDef pynumstore_module = {
    .m_base    = PyModuleDef_HEAD_INIT,
    .m_name    = "_pynumstore",
    .m_doc     = "Thin C wrapper around numstore for the pynumstore package.",
    .m_size    = -1,
    .m_methods = pynumstore_methods,
};

PyMODINIT_FUNC PyInit__pynumstore (void);

PyMODINIT_FUNC
PyInit__pynumstore (void)
{
  import_array ();
  return PyModule_Create (&pynumstore_module);
}
