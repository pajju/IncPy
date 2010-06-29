/* The FuncMemoInfo 'class'
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#include "Python.h" // really silly, but if you don't #include this
                    // before memoize_fmi.h, then gcc refuses to
                    // compile this ... my dependencies are majorly screwy :/
#include "memoize_fmi.h"
#include "code.h"
#include "memoize.h"
#include "memoize_logging.h"


FuncMemoInfo* deserialize_func_memo_info(PyObject* serialized_fmi, PyCodeObject* cod);


FuncMemoInfo* NEW_func_memo_info(PyCodeObject* cod) {
  FuncMemoInfo* new_fmi = PyMem_New(FuncMemoInfo, 1);
  // null out all fields
  memset(new_fmi, 0, sizeof(*new_fmi));

  // set your f_code pointer ...
  new_fmi->f_code = (PyObject*)cod;
  Py_INCREF(new_fmi->f_code);

  // then set f_code's pointer BACK to yourself
  cod->pg_func_memo_info = new_fmi;

  PyObject* cur_code_dependency =
    PyDict_GetItem(func_name_to_code_dependency, cod->pg_canonical_name);

  // if for some reason cod hasn't yet been entered into
  // func_name_to_code_dependency, then do it right now
  if (!cur_code_dependency) {
    PG_LOG_PRINTF("dict(event='WARNING', what='NEW_func_memo_info: cod not in func_name_to_code_dependency', name='%s')\n",
                      PyString_AsString(cod->pg_canonical_name));

    add_new_code_dep(cod);

    // now try again, and the lookup had better succeed!
    cur_code_dependency =
      PyDict_GetItem(func_name_to_code_dependency, cod->pg_canonical_name);
  }

  assert(cur_code_dependency);

  // create self-dependency:
  new_fmi->code_dependencies = PyDict_New();
  PyDict_SetItem(new_fmi->code_dependencies,
                 cod->pg_canonical_name, cur_code_dependency);

  return new_fmi;
}


void DELETE_func_memo_info(FuncMemoInfo* fmi) {
  Py_CLEAR(fmi->memoized_vals);
  Py_CLEAR(fmi->code_dependencies);
  Py_CLEAR(fmi->f_code);
  PyMem_Del(fmi);
}


void clear_cache_and_mark_pure(FuncMemoInfo* func_memo_info) {
  PG_LOG_PRINTF("dict(event='CLEAR_CACHE_AND_MARK_PURE', what='%s')\n",
                PyString_AsString(GET_CANONICAL_NAME(func_memo_info)));

  Py_CLEAR(func_memo_info->memoized_vals);

  // ugly hack to prevent IncPy from trying to load the now-stale
  // version of memoized_vals from disk and instead use the proper
  // value, which is NULL (the now-stale XXX.memoized_vals.pickle file
  // will be deleted at the end of execution)
  func_memo_info->memoized_vals_loaded = 1;

  Py_CLEAR(func_memo_info->code_dependencies);

  // re-insert self code dependency since you still have a dependency on
  // your own code.  Note that we are grabbing it from
  // func_name_to_code_dependency, which should be the most
  // recently-updated value from THIS execution
  // (we don't simply want to re-insert your original code dependency
  //  since that might be from a previous outdated version of your code)
  PyObject* new_self_code_dep = PyDict_GetItem(func_name_to_code_dependency,
                                               GET_CANONICAL_NAME(func_memo_info));
  assert(new_self_code_dep);

  func_memo_info->code_dependencies = PyDict_New();
  PyDict_SetItem(func_memo_info->code_dependencies,
                 GET_CANONICAL_NAME(func_memo_info), new_self_code_dep);

  // it's now pure again until proven otherwise
  func_memo_info->is_impure = 0;
  func_memo_info->likely_nothing_to_memoize = 0;
}


// Look for the appropriate func_memo_info entry from a code object,
// memory, and on-disk, or create a new one if it doesn't yet exist
FuncMemoInfo* get_func_memo_info_from_cod(PyCodeObject* cod) {
  // The order of the following steps really MATTERS!
  
  // FAST-PATH: first check if it's already in cod
  if (cod->pg_func_memo_info) {
    return cod->pg_func_memo_info;
  }

  // next check if it's already been loaded into memory
  PyObject* fmi_addr = PyDict_GetItem(all_func_memo_info_dict, cod->pg_canonical_name);
  if (fmi_addr) {
    return (FuncMemoInfo*)PyInt_AsLong(fmi_addr);
  }

  FuncMemoInfo* my_func_memo_info = NULL;

  // next, check to see whether it's stored in a .pickle file.
  PyObject* basename = hexdigest_str(cod->pg_canonical_name);
  PyObject* pickle_filename = PyString_FromFormat("incpy-cache/%s.dependencies.pickle",
                                         PyString_AsString(basename));
  Py_DECREF(basename);
  assert (pickle_filename);

  PyObject* pf = PyFile_FromString(PyString_AsString(pickle_filename), "r");
  Py_DECREF(pickle_filename);
  if (pf) {
    // note that calling cPickle_load_func might cause other
    // modules to be imported, since the pickle module loads
    // modules as-needed depending on the types of the objects
    // currently being unpickled
    PyObject* serialized_func_memo_info = PyObject_CallFunctionObjArgs(cPickle_load_func, pf, NULL);

    // only initialize my_func_memo_info if serialized_func_memo_info
    // can be properly unpickled
    if (!serialized_func_memo_info) {
      assert(PyErr_Occurred());
      PyErr_Clear();
    }
    else {
      my_func_memo_info = deserialize_func_memo_info(serialized_func_memo_info, cod);
      Py_DECREF(serialized_func_memo_info);
    }

    Py_DECREF(pf);
  }
  else {
    assert(PyErr_Occurred());
    PyErr_Clear();
  }

  // if it's not found, then create a fresh new FuncMemoInfo.
  if (!my_func_memo_info) {
    // clear the error code first
    PyErr_Clear();

    my_func_memo_info = NEW_func_memo_info(cod);
    assert(my_func_memo_info);

    // force writeback of memoized_vals (dunno if this is strictly
    // necessary, but just do it to be paranoid)
    my_func_memo_info->memoized_vals_loaded = 1;
  }

  // add its address to all_func_memo_info_dict:
  assert(my_func_memo_info);
  fmi_addr = PyInt_FromLong((long)my_func_memo_info);
  PyDict_SetItem(all_func_memo_info_dict, cod->pg_canonical_name, fmi_addr);
  Py_DECREF(fmi_addr);

  return my_func_memo_info;
}


// retrieve and possibly lazy-load fmi->memoized_vals
PyObject* get_memoized_vals_lst(FuncMemoInfo* fmi) {
  // lazy-load optimization:
  if (!fmi->memoized_vals_loaded) {
    assert(!fmi->memoized_vals);

    PyObject* basename = hexdigest_str(GET_CANONICAL_NAME(fmi));

    PyObject* memoized_vals_fn =
      PyString_FromFormat("incpy-cache/%s.memoized_vals.pickle",
                          PyString_AsString(basename));

    PyObject* memoized_vals_infile =
      PyFile_FromString(PyString_AsString(memoized_vals_fn), "r");

    if (memoized_vals_infile) {
      // this might be SLOW for a large memoized_vals_lst ...
      fmi->memoized_vals =
        PyObject_CallFunctionObjArgs(cPickle_load_func, memoized_vals_infile, NULL);

      if (!fmi->memoized_vals) {
        assert(PyErr_Occurred());
        PyErr_Clear();
        PG_LOG_PRINTF("dict(event='WARNING', what='cannot load memoized vals from disk', funcname='%s', filename='%s')\n",
                        PyString_AsString(GET_CANONICAL_NAME(fmi)),
                        PyString_AsString(memoized_vals_fn));
      }
      Py_DECREF(memoized_vals_infile);
    }
    else {
      assert(PyErr_Occurred());
      PyErr_Clear();
    }

    Py_DECREF(memoized_vals_fn);
    Py_DECREF(basename);

    // unconditionally set this to 1, so that we only attempt to load
    // from disk ONCE per execution
    fmi->memoized_vals_loaded = 1;
  }

  return fmi->memoized_vals;
}


// Given a FuncMemoInfo, create a serialized version of all of its
// dependencies as a newly-allocated dict (ready for pickling).
PyObject* serialize_func_memo_info_dependencies(FuncMemoInfo* func_memo_info) {
  PyObject* serialized_fmi = PyDict_New();

  assert(GET_CANONICAL_NAME(func_memo_info));
  PyDict_SetItemString(serialized_fmi,
                       "canonical_name",
                       GET_CANONICAL_NAME(func_memo_info));

  if (func_memo_info->code_dependencies) {
    PyDict_SetItemString(serialized_fmi,
                         "code_dependencies",
                         func_memo_info->code_dependencies);
  }

  return serialized_fmi;
}


// given a PyDict representing a serialized version of a FuncMemoInfo
// object and its corresponding code object, deserialize it and return a
// newly-allocated FuncMemoInfo object
FuncMemoInfo* deserialize_func_memo_info(PyObject* serialized_fmi, PyCodeObject* cod) {
  assert(PyDict_CheckExact(serialized_fmi));

  FuncMemoInfo* my_func_memo_info = NEW_func_memo_info(cod);


#ifdef ENABLE_DEBUG_LOGGING
  // sanity check to make sure the name matches:
  PyObject* serialized_canonical_name = 
    PyDict_GetItemString(serialized_fmi, "canonical_name");
  assert(serialized_canonical_name);
  assert(_PyString_Eq(serialized_canonical_name, cod->pg_canonical_name));
#endif // ENABLE_DEBUG_LOGGING


  // Optimization: leave memoized_vals null for now and lazy-load it from
  // disk when it's first needed (in get_memoized_vals_lst())
  my_func_memo_info->memoized_vals = NULL;


  PyObject* code_dependencies =
    PyDict_GetItemString(serialized_fmi, "code_dependencies");
  if (code_dependencies) {
    Py_INCREF(code_dependencies);
    my_func_memo_info->code_dependencies = code_dependencies;
  }

  return my_func_memo_info;
}

