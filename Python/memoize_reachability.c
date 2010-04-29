/* Determining reachability
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#include "memoize.h"
#include "memoize_logging.h"
#include "memoize_reachability.h"

/* Optimization:

  Note that we only care about tracking at most ONE global variable that
  contains a given value.  If a value is contained within more than one
  global variable (i.e., aliased), then we only need to maintain a
  dependency on ONE of them.  If that value is later mutated via the
  other aliases, it still shows up as a change to the variable that we
  added a dependency on.  e.g.,:

  z = Object()
  x.foo = z
  y.bar = z

  Let's say that x and y are globals.  Technically, the value referred
  to by z is reachable from both x and y, so a function that reads z
  should have global variable dependencies on both x and y.  However,
  it's sufficient to simply add a dependency on x or y but not both,
  since if the value of z is mutated through either x.foo or y.bar, it
  will show up as a change to both x and y.  It's simpler and more
  efficient to keep a dependency on at most ONE global variable rather
  than a set of global variables. */


/* Implement an interning optimization since there are often numerous
   references to very few symbols.  The contents of this cache are
   tuples representing the (compound) names of global variables that
   values are contained within.

   This cache is implemented as a DICT where the key and value refer to
   the same object, so we can simply add references to it in
   global_container_dict */
PyObject* global_containment_intern_cache = NULL;


/* Key:   PyLong representing the address of a PyObject
   Value: PyTuple representing global container of the key PyObject

   (this will work assuming that PyObjects never shift around in memory,
    which I believe is guaranteed by Include/object.h)

   This should only hold MUTABLE values (see
   update_global_container for more details on why) */
PyObject* global_container_dict = NULL;
// guard global_container_dict with a bloom filter to speed up lookups:
BLOOM* global_container_dict_keys_filter = NULL;


PyObject* get_global_container(PyObject* obj) {
  /* Optimization: check the bloom filter first to avoid constructing a
     PyLong and doing a full dict lookup.  note that if a false positive
     occurs, then that's okay because the real PyDict_GetItem lookup
     will always give the right answer: */
  if (!bloom_check(global_container_dict_keys_filter, (void*)obj)) {
    return NULL;
  }

  // we will hash the object's address, NOT the object itself ...
  PyObject* myAddr = PyLong_FromLong((long)obj);
  PyObject* ret = PyDict_GetItem(global_container_dict, myAddr);
  Py_DECREF(myAddr);
  return ret;
}

void set_global_container(PyObject* obj, PyObject* global_container) {
  // we will hash the object's address, NOT the object itself ...
  PyObject* myAddr = PyLong_FromLong((long)obj);
  PyDict_SetItem(global_container_dict, myAddr, global_container);
  Py_DECREF(myAddr);

  // don't forget to add to bloom filter:
  bloom_add(global_container_dict_keys_filter, (void*)obj);
}


/* Looks up the value of a global variable using cur_frame as a starting
   point for the search.
  
   varname_tuple is a tuple of strings:

   the first element is ALWAYS a filename indicating the file that
   this global variable was defined in

   the rest of the elements should be attributes of the f_globals dict
   of the module indicated by the filename given in the first element

   e.g., ('foo.py', 'global_lst') is a variable named global_lst defined
         in foo.py
   and   ('foo.py', 'second_module', 'global_lst') is a variable named
         second_module.global_lst defined in foo.py (most likely accessing 
         a value in another module named second_module, though)

    Returns NULL if the search fails */
PyObject* find_globally_reachable_obj_by_name(PyObject* varname_tuple,
                                              PyFrameObject* cur_frame) {
  assert(PyTuple_CheckExact(varname_tuple));
  assert(cur_frame);
  PyObject* ret = NULL;

  Py_ssize_t len = PyTuple_Size(varname_tuple);
  assert(len > 1); // no singletons allowed

  PyObject* globals_dict = NULL;

  PyObject* filename = PyTuple_GetItem(varname_tuple, 0);

  // if the filename is the SAME as cur_frame, then we can use
  // cur_frame->f_globals as the basis for our look-up
  if (_PyString_Eq(filename, cur_frame->f_code->co_filename)) {
    globals_dict = cur_frame->f_globals;
  }
  else {
    // otherwise, we have to look up the module associated with filename
    // and use f_globals from that module

    // our simple-ass algorithm is to take the LAST component of the
    // path and strip off the '.' extension.
    //
    // This assumes POSIX-style paths with '/' separators, so it might
    // not work on Windows systems ;)
    // 
    // e.g., convert '/Users/pgbovine/Desktop/my_module.pyc' into my_module
    PyObject* path_components_lst = PyObject_CallMethod(filename, "split", "s", "/");
    Py_ssize_t len = PyList_Size(path_components_lst);
    assert(len > 0);
    PyObject* basename = PyList_GET_ITEM(path_components_lst, len - 1); // does NOT incref

    PyObject* external_module_name_lst = PyObject_CallMethod(basename, "split", "s", ".");

    assert(PyList_Size(external_module_name_lst) == 2); // should be name.extension

    PyObject* external_module_name = PyList_GET_ITEM(external_module_name_lst, 0); // does NOT incref

    // start at cur_frame->f_globals and look up external_module_name.
    // we BETTER find this module, or else we must return NULL for FAIL!
    PyObject* external_module = PyDict_GetItem(cur_frame->f_globals, external_module_name);

    Py_DECREF(external_module_name_lst);
    Py_DECREF(path_components_lst);

    // if we can't find a module with that name, then simply defer back
    // to using cur_frame->f_globals in the hopes that the symbol was
    // imported into cur_frame's namespace via a "from <MODULE> import X"
    // command (rather than an "import <MODULE>" command)
    if (!external_module || !(PyModule_Check(external_module))) {
      globals_dict = cur_frame->f_globals;
    }
    else {
      // otherwise remember to grab the DICT of the module, and not the
      // module itself!
      globals_dict = PyModule_GetDict(external_module);
    }
  }
  assert(globals_dict);

  PyObject* cur_attrname = PyTuple_GetItem(varname_tuple, 1);

  ret = PyDict_GetItem(globals_dict, cur_attrname);

  // we can't even find the FIRST global object, then ... PUNT!
  if (!ret) return NULL;

  Py_ssize_t i;
  // start at 2, since 0 is the filename and 1 is cur_attrname!
  for (i = 2; i < len; i++) {
    cur_attrname = PyTuple_GetItem(varname_tuple, i);
    ret = PyObject_GetAttr(ret, cur_attrname);

    // if at any time can't find an attribute, then PUNT!
    if (!ret) {
      // clear the error code ...
      assert(PyErr_Occurred());
      PyErr_Clear();
      return NULL;
    }
  }

  return ret;
}


// returns a NEWLY-ALLOCATED tuple that represents the concatenation of 
// the old_elt tuple with new_str.
static PyObject* extend_tuple(PyObject* old_elt, PyObject* new_str) {
  assert(PyString_CheckExact(new_str));

  PyObject* new_tup = NULL;

  // if it's already a tuple, then extend the existing tuple by 
  //  appending attrname to the end of
  // it, in order to form (___, ___, ___, ..., attrname):
  assert(PyTuple_CheckExact(old_elt));
  Py_ssize_t len = PyTuple_Size(old_elt);

  new_tup = PyTuple_New(len + 1);

  Py_ssize_t i;
  for (i = 0; i < len; i++) {
    PyObject* elt = PyTuple_GET_ITEM(old_elt, i);
    Py_INCREF(elt);
    PyTuple_SET_ITEM(new_tup, i, elt);
  }
  Py_INCREF(new_str);
  PyTuple_SET_ITEM(new_tup, len, new_str);

  return new_tup;
}


// returns a new tuple, implementing interning optimization
PyObject* create_varname_tuple(PyObject* filename, PyObject* varname) {
  assert(PyString_CheckExact(filename));
  assert(PyString_CheckExact(varname));
  PyObject* new_tup = PyTuple_Pack(2, filename, varname);

  PyObject* interned_tup = PyDict_GetItem(global_containment_intern_cache, new_tup);
  if (!interned_tup) {
    PyDict_SetItem(global_containment_intern_cache, new_tup, new_tup);
    interned_tup = new_tup;
  }
  Py_DECREF(new_tup);

  return interned_tup;
}

// assuming parent is a globally-reachable object, return a NEW global 
// var tuple suitable for assignment to global_container_dict,
// formed by extending each elt. of get_global_container(parent)
// with attrname (also implements interning optimization)
//
// this does NOT increment refcount of the newly-created value, since
// it uses interning optimization.  caller does NOT have to decref
PyObject* extend_with_attrname(PyObject* parent, PyObject* attrname) {
  PyObject* parent_container = get_global_container(parent);
  assert(parent_container && PyTuple_CheckExact(parent_container));

  PyObject* result = extend_tuple(parent_container, attrname);
  assert(result);

  PyObject* interned_result = PyDict_GetItem(global_containment_intern_cache, result);
  if (!interned_result) {
    PyDict_SetItem(global_containment_intern_cache, result, result);
    interned_result = result;
  }
  Py_DECREF(result);

  return interned_result;
}


/* if obj does not already have an entry in global_container_dict, then
   point it to new_elt.  if it already has one, then DO NOTHING!  (see
   the 'Optimization' note at the top of this file for why we do this)

   WE ONLY DO THIS FOR MUTABLE OBJECTS! */
void update_global_container(PyObject* obj, PyObject* new_elt) {
  // VERY IMPORTANT but subtle point - immutable values might be
  // interned by the Python interpreter implementation (e.g., small
  // integers are interned), so we don't want to taint them by adding
  // them to global_container_dict
  if (DEFINITELY_IMMUTABLE(obj)) {
    return;
  }

  PyObject* cur_container = get_global_container(obj);

  if (!cur_container) {
    set_global_container(obj, new_elt);
  }
}


// returns 1 iff obj contains within it a MUTABLE object that
// existed before the invocation of frame f, 0 otherwise
//
// TODO: this will TOTALLY infinite-loop on an object with circular
// references ... let's deal with this later!!!
int contains_externally_aliased_mutable_obj(PyObject* obj, PyFrameObject* f) {
  if (DEFINITELY_IMMUTABLE(obj)) {
    return 0;
  }

  // this is a hacky-hack ... tuples are immutable, so we don't need
  // to check their creation time.  however, they can hold mutable
  // elements, so we still need to traverse inside of them.
  // e.g., ([1, 2], [3]) is a legal tuple
  //
  // tuples can appear as constants in function code (co_consts),
  // so those are actually created BEFORE a function is called but
  // are harmless since they can't contain mutable items inside.
  if (!PyTuple_CheckExact(obj)) {
    // STINT - I don't yet support Py_CREATION_TIME in this branch ...

    /*
    // only do the time check on MUTABLE items
    if (Py_CREATION_TIME(obj) < f->start_instr_time) {
      return 1;
    }
    */
    return 0;
  }

  // otherwise recurse inside of yourself to get your constituent
  // sub-objects (note: this will NOT traverse inside of extension
  // types defined as C code, only pure Python types)

  /* if it's a built-in collection type (list, tuple, dict, set), then
     recurse on all elements. (TODO: might be slow) */
  if (PyList_Check(obj)) {
    Py_ssize_t i;
    for (i = 0; i < PyList_Size(obj); i++) {
      int sub_res = contains_externally_aliased_mutable_obj(PyList_GetItem(obj, i), f);
      // break out as soon as you find just ONE element
      // that's globally-reachable
      if (sub_res) return 1;
    }
  }
  else if (PyTuple_Check(obj)) {
    Py_ssize_t i;
    for (i = 0; i < PyTuple_Size(obj); i++) {
      int sub_res = contains_externally_aliased_mutable_obj(PyTuple_GetItem(obj, i), f);
      if (sub_res) return 1;
    }
  }
  else if (PySet_Check(obj)) {
    Py_ssize_t pos = 0;
    PyObject* child;
    while (_PySet_Next(obj, &pos, &child)) {
      int sub_res = contains_externally_aliased_mutable_obj(child, f);
      if (sub_res) return 1;
    }
  }
  else if (PyDict_Check(obj)) {
    PyObject* key = NULL;
    PyObject* value = NULL;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key, &value)) {
      int key_res = contains_externally_aliased_mutable_obj(key, f);
      int value_res = contains_externally_aliased_mutable_obj(value, f);
      if (key_res || value_res) return 1;
    }
  }
  // instance of a user-defined class ... dig inside its attributes dict
  else if (PyInstance_Check(obj)) {
    PyInstanceObject* inst = (PyInstanceObject*)obj;
    return contains_externally_aliased_mutable_obj(inst->in_dict, f);
  }


  // if you've made it this far without tripping any alarms, then let's 
  // assume that you don't contain any externally-aliased values
  return 0;
}

