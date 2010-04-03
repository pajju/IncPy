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

  new_fmi->code_dependencies = PyDict_New();

  PyObject* cur_code_dependency = 
    PyDict_GetItem(func_name_to_code_dependency, cod->pg_canonical_name);

  // create self-dependency:
  if (cur_code_dependency) {
    PyDict_SetItem(new_fmi->code_dependencies, 
                   cod->pg_canonical_name, cur_code_dependency);
  }

  return new_fmi;
}


void DELETE_func_memo_info(FuncMemoInfo* fmi) {
  Py_CLEAR(fmi->memoized_vals);
  Py_CLEAR(fmi->code_dependencies);
  Py_CLEAR(fmi->global_var_dependencies);
  Py_CLEAR(fmi->file_read_dependencies);
  Py_CLEAR(fmi->file_write_dependencies);
  Py_CLEAR(fmi->f_code);

  PyMem_Del(fmi);
}


void clear_cache_and_mark_pure(FuncMemoInfo* func_memo_info) {
  PG_LOG_PRINTF("dict(event='CLEAR_CACHE_AND_MARK_PURE', what='%s')\n",
                PyString_AsString(GET_CANONICAL_NAME(func_memo_info)));

  Py_CLEAR(func_memo_info->memoized_vals);
  Py_CLEAR(func_memo_info->code_dependencies);
  Py_CLEAR(func_memo_info->global_var_dependencies);
  Py_CLEAR(func_memo_info->file_read_dependencies);
  Py_CLEAR(func_memo_info->file_write_dependencies);

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

  // force a writeback
  func_memo_info->do_writeback = 1;
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
    return (FuncMemoInfo*)PyLong_AsLong(fmi_addr);
  }

  FuncMemoInfo* my_func_memo_info = NULL;

  // next, check to see whether it's stored in a .pickle file.

  // consult pickle_filenames for the mapping between canonical_name
  // and .pickle filename
  PyObject* pickle_filename = PyDict_GetItem(pickle_filenames, cod->pg_canonical_name);

  if (pickle_filename) {
    // this file had better exist!
    PyObject* pf = PyFile_FromString(PyString_AsString(pickle_filename), "r");
    if (pf) {
      // note that calling cPickle_load_func might cause other
      // modules to be imported, since the pickle module loads
      // modules as-needed depending on the types of the objects
      // currently being unpickled
      PyObject* tup = PyTuple_Pack(1, pf);
      PyObject* serialized_func_memo_info = PyObject_Call(cPickle_load_func, tup, NULL);
      assert(serialized_func_memo_info);

      my_func_memo_info = deserialize_func_memo_info(serialized_func_memo_info, cod);

      Py_DECREF(serialized_func_memo_info);
      Py_DECREF(tup);
      Py_DECREF(pf);
    }
    else {
      // croak if pickle file not found
      assert(PyErr_Occurred());
      PyErr_Print();
      PG_LOG_PRINTF("dict(event='ERROR', why='PICKLE_FILE_NOT_FOUND', what='%s')\n",
                    PyString_AsString(cod->pg_canonical_name));
      exit(1);
    }
  }

  // if it's not found, then create a fresh new FuncMemoInfo.
  if (!my_func_memo_info) {
    // clear the error code first
    PyErr_Clear();

    my_func_memo_info = NEW_func_memo_info(cod);

    // set writeback since it's a brand-new entry
    my_func_memo_info->do_writeback = 1;
  }

  // add its address to all_func_memo_info_dict:
  assert(my_func_memo_info);
  fmi_addr = PyLong_FromLong((long)my_func_memo_info);
  PyDict_SetItem(all_func_memo_info_dict, cod->pg_canonical_name, fmi_addr);
  Py_DECREF(fmi_addr);

  return my_func_memo_info;
}


// Given a FuncMemoInfo, create a serialized version as a
// newly-allocated dict.  The serialized version should be ready for
// pickling.
PyObject* serialize_func_memo_info(FuncMemoInfo* func_memo_info) {
  PyObject* serialized_fmi = PyDict_New();

  assert(GET_CANONICAL_NAME(func_memo_info));
  PyDict_SetItemString(serialized_fmi,
                       "canonical_name",
                       GET_CANONICAL_NAME(func_memo_info));

  if (func_memo_info->memoized_vals) {
    PyDict_SetItemString(serialized_fmi,
                         "memoized_vals",
                         func_memo_info->memoized_vals);
  }

  if (func_memo_info->global_var_dependencies) {
    PyDict_SetItemString(serialized_fmi,
                         "global_var_dependencies",
                         func_memo_info->global_var_dependencies);
  }

  if (func_memo_info->code_dependencies) {
    PyDict_SetItemString(serialized_fmi,
                         "code_dependencies",
                         func_memo_info->code_dependencies);
  }

  if (func_memo_info->file_read_dependencies) {
    PyDict_SetItemString(serialized_fmi,
                         "file_read_dependencies",
                         func_memo_info->file_read_dependencies);
  }

  if (func_memo_info->file_write_dependencies) {
    PyDict_SetItemString(serialized_fmi,
                         "file_write_dependencies",
                         func_memo_info->file_write_dependencies);
  }

  return serialized_fmi;
}


// given a PyDict representing a serialized version of a FuncMemoInfo
// object and its corresponding code object, deserialize it and return a
// newly-allocated FuncMemoInfo object
FuncMemoInfo* deserialize_func_memo_info(PyObject* serialized_fmi, PyCodeObject* cod) {
  assert(PyDict_CheckExact(serialized_fmi));

  FuncMemoInfo* my_func_memo_info = NEW_func_memo_info(cod);

  // sanity check to make sure the name matches:
  PyObject* serialized_canonical_name = 
    PyDict_GetItemString(serialized_fmi, "canonical_name");
  assert(serialized_canonical_name);
  assert(_PyString_Eq(serialized_canonical_name, cod->pg_canonical_name));

  PyObject* memoized_vals = 
    PyDict_GetItemString(serialized_fmi, "memoized_vals");
  if (memoized_vals) {
    Py_INCREF(memoized_vals);
    my_func_memo_info->memoized_vals = memoized_vals;
  }

  PyObject* global_var_dependencies = 
    PyDict_GetItemString(serialized_fmi, "global_var_dependencies");
  if (global_var_dependencies) {
    Py_INCREF(global_var_dependencies);
    my_func_memo_info->global_var_dependencies = global_var_dependencies;
  }

  PyObject* code_dependencies = 
    PyDict_GetItemString(serialized_fmi, "code_dependencies");
  if (code_dependencies) {
    Py_INCREF(code_dependencies);
    my_func_memo_info->code_dependencies = code_dependencies;
  }

  PyObject* file_read_dependencies = 
    PyDict_GetItemString(serialized_fmi, "file_read_dependencies");
  if (file_read_dependencies) {
    Py_INCREF(file_read_dependencies);
    my_func_memo_info->file_read_dependencies = file_read_dependencies;
  }

  PyObject* file_write_dependencies = 
    PyDict_GetItemString(serialized_fmi, "file_write_dependencies");
  if (file_write_dependencies) {
    Py_INCREF(file_write_dependencies);
    my_func_memo_info->file_write_dependencies = file_write_dependencies;
  }

  return my_func_memo_info;
}

