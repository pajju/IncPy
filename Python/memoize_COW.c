/* Copy-on-write optimization
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#include "memoize.h"
#include "memoize_COW.h"
#include "memoize_logging.h"
#include "memoize_reachability.h"


// boring forward decls:
static void add_all_contained_mutable_object_addrs(PyObject* obj,
                                                   PyObject* contained_addrs_set);

/*
  
  To implement COW (copy-on-write) optimization, we need to track
  whether an object has been mutated (or one of its sub-objects has been
  mutated)

  Add an entry into cow_containment_dict when you are supposed to do a
  deepcopy but want to defer it until the object has been modified.

  Remove the entry after you do the COW, since the key might no longer
  be the address of an alive object

  Key: long int representing an address of a PyObject we want to COW

  Value: a set of long ints representing addresses of all MUTABLE
  objects within the key PyObject

*/
PyObject* cow_containment_dict = NULL;

// Optimization: store all values contained in cow_containment_dict in
// here, so that we can quickly check whether an address is in ANY
// member of cow_containment_dict without iterating through it all:
PyObject* cow_traced_addresses_set = NULL;


/*

  Given a PyObject obj, add its own address to contained_addrs_set, and
  then traverse inside of itself to recursively add addresses of all
  MUTABLE enclosing objects

  Rules:

    immutable primitive - STOP
    list/tuple/set - add itself and traverse inside all elements
    dict - add itself and traverse inside all keys and values
           (yes, even keys can be mutable objects!)
    object instance - add itself and traverse inside its __dict__ fields
    any other type - STOP and issue an unsoundness warning since
                     we don't know how to traverse inside
                     (e.g., a C extension object)

  (to handle circular references and prevent infinite loops, iterate
  until fixpoint)

  possibly MUTATES contained_addrs_set on every iteration

*/
static void add_all_contained_mutable_object_addrs(PyObject* obj,
                                                   PyObject* contained_addrs_set) {
  // BAIL!
  if (DEFINITELY_IMMUTABLE(obj)) {
    return;
  }

  // we will hash the object's address, NOT the object itself ...
  PyObject* myAddr = PyLong_FromLong((long)obj);

  // fixpoint reached!  if you're already in the set, then there's no
  // point in recursing into youself AGAIN (might be an infinite loop 
  // due to a circular reference)
  if (PySet_Contains(contained_addrs_set, myAddr)) {
    return;
  }

  // add yourself!
  PySet_Add(contained_addrs_set, myAddr);
  Py_DECREF(myAddr);


  // now recurse inside of yourself if possible, and if not, issue an
  // UNSOUNDNESS WARNING ...

  if (PyList_Check(obj)) {
    Py_ssize_t i;
    for (i = 0; i < PyList_Size(obj); i++) {
      add_all_contained_mutable_object_addrs(PyList_GET_ITEM(obj, i), contained_addrs_set);
    }
  }
  else if (PyTuple_Check(obj)) {
    Py_ssize_t i;
    for (i = 0; i < PyTuple_Size(obj); i++) {
      add_all_contained_mutable_object_addrs(PyTuple_GET_ITEM(obj, i), contained_addrs_set);
    }
  }
  else if (PySet_Check(obj)) {
    Py_ssize_t pos = 0;
    PyObject* child;
    while (_PySet_Next(obj, &pos, &child)) {
      add_all_contained_mutable_object_addrs(child, contained_addrs_set);
    }
  }
  else if (PyDict_Check(obj)) {
    PyObject* key = NULL;
    PyObject* value = NULL;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key, &value)) {
      add_all_contained_mutable_object_addrs(key, contained_addrs_set);
      add_all_contained_mutable_object_addrs(value, contained_addrs_set);
    }
  }
  else if (PyInstance_Check(obj)) {
    // for instance objects, traverse inside of their dicts ...
    PyInstanceObject* inst = (PyInstanceObject*)obj;
    add_all_contained_mutable_object_addrs(inst->in_dict, contained_addrs_set);
  }
  else if (PyObject_HasAttrString(obj, "__dict__")) {
    // if the object has a __dict__ element, then traverse inside of it:
    PyObject* obj_dict = PyObject_GetAttrString(obj, "__dict__");
    assert(obj_dict);
    add_all_contained_mutable_object_addrs(obj_dict, contained_addrs_set);
  }
  else {
    PG_LOG_PRINTF("dict(event='WARNING', what='UNSOUNDNESS', why='Cannot traverse inside of obj for COW mutation tracking', type='%s')\n", Py_TYPE(obj)->tp_name);
  }
}

void cow_containment_dict_ADD(PyObject* obj) {
  // really fast-pass ... if it's immutable, no need to track anything!
  if (DEFINITELY_IMMUTABLE(obj)) {
    return;
  }

  PyObject* myAddr = PyLong_FromLong((long)obj);

  // if it's already in cow_containment_dict, then don't bother to
  // traverse through it AGAIN ...
  if (PyDict_Contains(cow_containment_dict, myAddr)) {
    return;
  }

  PyObject* s = PySet_New(NULL);
  add_all_contained_mutable_object_addrs(obj, s);

  // only add it if the set contains SOME elements.  otherwise there's
  // no point in bothering!
  if (PySet_Size(s) > 0) {
    PyDict_SetItem(cow_containment_dict, myAddr, s);
    _PySet_Update(cow_traced_addresses_set, s); // remember to update cache too!
  }

  Py_DECREF(s);
  Py_DECREF(myAddr);
}


/*

  obj_addr is a PyLong object that represents the ADDRESS of the object
  that is ABOUT TO BE MUTATED (but hasn't been mutated yet, due to
  pg_about_to_MUTATE_event being called BEFORE the mutation actually
  occurs)

  Returns 1 on success and 0 on failure

 */
static int do_COW_and_update_refs(PyObject* obj_addr) {
  // wow what a dangerous cast, but let's pray that it works properly ;)
  long addrLong = PyLong_AsLong(obj_addr);
  PyObject* objPtr = (PyObject*)addrLong;

  /* in some crazy cases (e.g., scipy.test()), the reference count is
     actually 0 because the object referred by obj_addr has already been
     freed.  let's croak on those cases for now, since i don't feel like
     diagnosing them yet. */
  assert(Py_REFCNT(objPtr) > 0);

  PyObject* copy = deepcopy(objPtr);

  if (!copy) {
    assert(PyErr_Occurred());
    //PyErr_Print();
    PyErr_Clear();

    /*

  TODO: Bailing out here will leave dangling references to unpicklable
  objects, which might cause pickling to fail

    */
    PG_LOG_PRINTF("dict(event='WARNING', what='CANNOT DEEPCOPY in do_COW_and_update_refs(); FuncMemoInfo might still contain unpicklable objects', type='%s')\n", Py_TYPE(objPtr)->tp_name);
    return 0;
  }

  /*

    check for references to objPtr in these structures for ALL functions:

      - memoized args
      - memoized retval
      - global_var_dependencies

    and REPLACE them with references to copy

    This might seem inefficient, but it's necessary to ensure
    correctness since there might be multiple references from
    func_memo_info metadata to COW'ed objects

    However, hopefully this is a rare occurrence, so it's okay that it's
    sorta expensive

  */

  PyObject* canonical_name = NULL;
  PyObject* fmi_addr = NULL;
  Py_ssize_t pos = 0;
  while (PyDict_Next(all_func_memo_info_dict, &pos, &canonical_name, &fmi_addr)) {
    long fmi_addr_long = PyLong_AsLong(fmi_addr);
    FuncMemoInfo* func_memo_info = (FuncMemoInfo*)fmi_addr_long;

    // first check global_var_dependencies
    PyObject* global_var_deps = func_memo_info->global_var_dependencies;

    if (global_var_deps) {
      PyObject* varname = NULL;
      PyObject* val = NULL;
      Py_ssize_t pos = 0;
      while (PyDict_Next(global_var_deps, &pos, &varname, &val)) {
        // replace reference to objPtr with a reference to copy
        if (val == objPtr) {
          // i think it's okay to do a PyDict_SetItem while iterating
          // through it ... but double-check later to make sure (TODO)
          PyDict_SetItem(global_var_deps, varname, copy);
        }
      }
    }


    // then check memoized_vals for memoized args and retval:
    PyObject* memoized_vals_lst = func_memo_info->memoized_vals;

    if (memoized_vals_lst) {
      Py_ssize_t i;
      for (i = 0; i < PyList_Size(memoized_vals_lst); i++) {
        PyObject* elt = PyList_GET_ITEM(memoized_vals_lst, i);
        assert(PyDict_CheckExact(elt));

        PyObject* args_lst = PyDict_GetItemString(elt, "args");
        PyObject* retval_lst = PyDict_GetItemString(elt, "retval");

        assert(PyList_Check(args_lst));
        Py_ssize_t j;
        for (j = 0; j < PyList_Size(args_lst); j++) {
          PyObject* val = PyList_GET_ITEM(args_lst, j);
          if (val == objPtr) {
            Py_INCREF(copy); // ugh stupid refcounts - PyList_SetItem doesn't inc for us
            PyList_SetItem(args_lst, j, copy);
          }
        }

        assert(PyList_Check(retval_lst));
        assert(PyList_Size(retval_lst) == 1);

        if (PyList_GET_ITEM(retval_lst, 0) == objPtr) {
          Py_INCREF(copy); // ugh stupid refcounts - PyList_SetItem doesn't inc for us
          PyList_SetItem(retval_lst, 0, copy);
        }
      }
    }
  }

  Py_DECREF(copy); // subtle but necessary to prevent leaks, I think

  return 1;
}

void check_COW_mutation(PyObject* obj) {
  PyObject* myAddr = PyLong_FromLong((long)obj);

  // fast-path return for common case
  if (!PySet_Contains(cow_traced_addresses_set, myAddr)) {
    Py_DECREF(myAddr);
    return;
  }

  PyObject* base_obj_addr = NULL;
  PyObject* contained_addrs_set = NULL;
  Py_ssize_t pos = 0;

  PyObject* elts_to_remove = PySet_New(NULL);

  // check whether obj is contained WITHIN any COW-saved base_obj_addr
  while (PyDict_Next(cow_containment_dict, &pos, &base_obj_addr, &contained_addrs_set)) {
    if (PySet_Contains(contained_addrs_set, myAddr)) {
      // if so, do an actual deepcopy and adjust all outstanding references
      if (do_COW_and_update_refs(base_obj_addr)) {
        // REMOVE elements after a successful COWing, since these
        // addresses might now be stale
        PySet_Add(elts_to_remove, base_obj_addr);
      }
    }
  }
  Py_DECREF(myAddr);

  if (PySet_Size(elts_to_remove) > 0) {
    Py_ssize_t pos = 0;
    PyObject* base_obj_addr_to_remove;
    while (_PySet_Next(elts_to_remove, &pos, &base_obj_addr_to_remove)) {
      PyDict_DelItem(cow_containment_dict, base_obj_addr_to_remove);
    }
  }

  Py_DECREF(elts_to_remove);
}

