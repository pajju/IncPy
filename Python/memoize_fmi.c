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

#include <dirent.h>
#include <unistd.h>


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

  PyObject* subdir_basename = hexdigest_str(GET_CANONICAL_NAME(new_fmi));
  new_fmi->cache_subdirectory_path =
    PyString_FromFormat("incpy-cache/%s.cache", PyString_AsString(subdir_basename));
  Py_DECREF(subdir_basename);

  return new_fmi;
}


void DELETE_func_memo_info(FuncMemoInfo* fmi) {
  Py_CLEAR(fmi->code_dependencies);
  Py_CLEAR(fmi->f_code);
  Py_CLEAR(fmi->cache_subdirectory_path);
  PyMem_Del(fmi);
}


void clear_cache_and_mark_pure(FuncMemoInfo* func_memo_info) {
  PG_LOG_PRINTF("dict(event='CLEAR_CACHE_AND_MARK_PURE', what='%s')\n",
                PyString_AsString(GET_CANONICAL_NAME(func_memo_info)));

  // erase the entire sub-directory of cache entries associated with func_memo_info
  assert(func_memo_info->cache_subdirectory_path);
  char* subdir_path_str = PyString_AsString(func_memo_info->cache_subdirectory_path);

  DIR* dp = opendir(subdir_path_str);
  if (dp) {
    struct dirent* dirp;
    while ((dirp = readdir(dp)) != NULL) {
      if ((strcmp(dirp->d_name, ".") != 0) && (strcmp(dirp->d_name, "..") != 0)) {
        PyObject* file_path =
          PyString_FromFormat("%s/%s", subdir_path_str, dirp->d_name);
        unlink(PyString_AsString(file_path));
      }
    }
    rmdir(subdir_path_str);
    closedir(dp);
  }

  func_memo_info->on_disk_cache_empty = 1;


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
  func_memo_info->all_code_deps_SAT = 0;
  func_memo_info->num_fast_calls_with_no_memoized_vals = 0;
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
  assert(pickle_filename);
  Py_DECREF(basename);

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

      struct stat st;
      if (stat(PyString_AsString(my_func_memo_info->cache_subdirectory_path), &st) != 0) {
        my_func_memo_info->on_disk_cache_empty = 1;
      }
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
    my_func_memo_info->on_disk_cache_empty = 1;

    assert(my_func_memo_info);
  }

  // add its address to all_func_memo_info_dict:
  assert(my_func_memo_info);
  fmi_addr = PyInt_FromLong((long)my_func_memo_info);
  PyDict_SetItem(all_func_memo_info_dict, cod->pg_canonical_name, fmi_addr);
  Py_DECREF(fmi_addr);

  return my_func_memo_info;
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


  PyObject* code_dependencies =
    PyDict_GetItemString(serialized_fmi, "code_dependencies");
  if (code_dependencies) {
    Py_INCREF(code_dependencies);
    my_func_memo_info->code_dependencies = code_dependencies;
  }

  return my_func_memo_info;
}


/* The on-disk persistent cache corresponding to each function looks
   like the following:

   Each 'key' is an md5 hash of cPickle.dumps([argument list])

   Each 'value' is a LIST of dicts, each containing the following fields:

     "args" --> argument list
     "global_vars_read" --> dict mapping global vars to values (OPTIONAL)

     "files_read" --> dict mapping files read to modtimes (OPTIONAL)
     "files_written" --> dict mapping files written to modtimes (OPTIONAL)

     "retval" --> return value

     "stdout_buf" --> buffered stdout string (OPTIONAL)
     "stderr_buf" --> buffered stderr string (OPTIONAL)
     "final_file_seek_pos" --> dict mapping filenames to their seek
                               positions at function exit time (OPTIONAL)

     "runtime_ms" --> how many milliseconds it took to run

   (the reason why this is a list rather than a single dict is because
   there could be MULTIPLE valid matches for a particular argument list,
   due to differing global variable values)

   Each function stores its persistent cache in its own sub-directory:

     incpy-cache/<hash of function name>.cache/

   and each 'value' is serialized in a pickle file named by the key:
 
     incpy-cache/<hash of function name>.cache/<hash of key>.pickle

*/


// Retrieves, de-serializes, and returns the entry associated with hash_key
// in the file (returning NULL if not found or unpickling error)
PyObject* on_disk_cache_GET(FuncMemoInfo* fmi, PyObject* hash_key) {
  assert(hash_key);
  assert(fmi->cache_subdirectory_path);

  // Optimization:
  if (fmi->on_disk_cache_empty) {
    return NULL;
  }

  PyObject* pickle_filename =
    PyString_FromFormat("%s/%s.pickle",
                        PyString_AsString(fmi->cache_subdirectory_path),
                        PyString_AsString(hash_key));
  PyObject* pf = PyFile_FromString(PyString_AsString(pickle_filename), "r");
  Py_DECREF(pickle_filename);

  if (pf) {
    PyObject* ret = PyObject_CallFunctionObjArgs(cPickle_load_func, pf, NULL);
    Py_DECREF(pf);

    if (ret) {
      return ret;
    }
    else {
      assert(PyErr_Occurred());
      PyErr_Clear();
      PG_LOG_PRINTF("dict(event='ERROR', what='Cannot unpickle cache entry', funcname='%s')\n",
                    PyString_AsString(GET_CANONICAL_NAME(fmi)));
      return NULL;
    }
  }
  else {
    // silently return NULL if cache file isn't found
    assert(PyErr_Occurred());
    PyErr_Clear();

    return NULL;
  }
}


// puts contents into persistent cache under a filename determined by
// hash_key
//
// returns the result of the pickling attempt (so that caller can
// error-check in the regular manner)
PyObject* on_disk_cache_PUT(FuncMemoInfo* fmi, PyObject* hash_key, PyObject* contents) {
  assert(hash_key);
  assert(fmi->cache_subdirectory_path);
  char* subdir_path_str = PyString_AsString(fmi->cache_subdirectory_path);

  // first create parent directories if they don't yet exist
  struct stat st;
  if (stat("incpy-cache", &st) != 0) {
    mkdir("incpy-cache", 0777);
  }

  if (stat(subdir_path_str, &st) != 0) {
    mkdir(subdir_path_str, 0777);
  }

  PyObject* pickle_filename =
    PyString_FromFormat("%s/%s.pickle",
                        subdir_path_str,
                        PyString_AsString(hash_key));
  PyObject* pickle_outfile = PyFile_FromString(PyString_AsString(pickle_filename), "wb");
  assert(pickle_outfile);
  Py_DECREF(pickle_filename);

  PyObject* negative_one = PyInt_FromLong(-1);
  PyObject* cPickle_dump_res =
    PyObject_CallFunctionObjArgs(cPickle_dump_func,
                                 contents,
                                 pickle_outfile,
                                 negative_one, NULL);
  Py_DECREF(negative_one);
  Py_DECREF(pickle_outfile);

  // For optimization purposes ... if the PUT succeeded, then the cache
  // is no longer empty
  if (cPickle_dump_res) {
    fmi->on_disk_cache_empty = 0;
  }

  return cPickle_dump_res;
}

void on_disk_cache_DEL(FuncMemoInfo* fmi, PyObject* hash_key) {
  assert(hash_key);
  assert(fmi->cache_subdirectory_path);
  char* subdir_path_str = PyString_AsString(fmi->cache_subdirectory_path);

  PyObject* pickle_filename =
    PyString_FromFormat("%s/%s.pickle",
                        subdir_path_str,
                        PyString_AsString(hash_key));

  // delete the cache file
  unlink(PyString_AsString(pickle_filename));

  Py_DECREF(pickle_filename);

  // if rmdir succeeds, then that means that there were NO other cache
  // entries left in the directory, so fmi->on_disk_cache_empty should
  // be set
  rmdir(subdir_path_str);

  struct stat st;
  if (stat(subdir_path_str, &st) != 0) {
      fmi->on_disk_cache_empty = 1;
  }
}

