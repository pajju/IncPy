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
  Py_CLEAR(fmi->impure_status_msg);
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
  func_memo_info->num_fast_calls_with_no_memoized_vals = 0;
  Py_CLEAR(func_memo_info->impure_status_msg);
}


// Look for the appropriate func_memo_info entry from a code object,
// in memory, or create a new one if it doesn't yet exist
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
  else {
    // otherwise, create a fresh new entry and add it to
    // all_func_memo_info_dict:

    FuncMemoInfo* new_fmi = NEW_func_memo_info(cod);

    // set on_disk_cache_empty depending on whether cache sub-directory exists
    assert(new_fmi->cache_subdirectory_path);
    struct stat st;
    if (stat(PyString_AsString(new_fmi->cache_subdirectory_path), &st) != 0) {
      new_fmi->on_disk_cache_empty = 1;
    }

    // add its address to all_func_memo_info_dict:
    assert(new_fmi);
    fmi_addr = PyInt_FromLong((long)new_fmi);
    PyDict_SetItem(all_func_memo_info_dict, cod->pg_canonical_name, fmi_addr);
    Py_DECREF(fmi_addr);

    return new_fmi;
  }
}


/* The on-disk persistent cache corresponding to each function looks
   like the following:

   Each 'key' is an md5 hash of cPickle.dumps([argument list])

   Each 'value' is a LIST of dicts, each containing the following fields:

     "canonical_name" --> name of function

     "args" --> argument list
     "global_vars_read" --> dict mapping global vars to values (OPTIONAL)

     "code_dependencies" --> dict mapping function names to code 'objects'

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

  // write to a temporary filename, then atomically rename it
  // to its proper filename when pickling successfully completed,
  // so that .pickle files are ALWAYS seen in a consistent state
  PyObject* pickle_tmp_filename =
    PyString_FromFormat("%s/%s.pickle.partial",
                        subdir_path_str,
                        PyString_AsString(hash_key));

  PyObject* pickle_outfile =
    PyFile_FromString(PyString_AsString(pickle_tmp_filename), "wb");
  assert(pickle_outfile);
  PyObject* negative_one = PyInt_FromLong(-1);
  PyObject* cPickle_dump_res =
    PyObject_CallFunctionObjArgs(cPickle_dump_func,
                                 contents,
                                 pickle_outfile,
                                 negative_one, NULL);
  Py_DECREF(negative_one);
  Py_DECREF(pickle_outfile);

  if (cPickle_dump_res) {
    // For optimization purposes ... if the PUT succeeded, then the
    // cache is no longer empty
    fmi->on_disk_cache_empty = 0;

    // atomically rename to its permanent filename
    PyObject* pickle_filename =
      PyString_FromFormat("%s/%s.pickle",
                          subdir_path_str,
                          PyString_AsString(hash_key));

    rename(PyString_AsString(pickle_tmp_filename), PyString_AsString(pickle_filename));
    Py_DECREF(pickle_filename);
  }
  else {
    // if the PUT failed, then just to be safe, delete the file so that
    // we don't try to unpickle an inconsistent file
    unlink(PyString_AsString(pickle_tmp_filename));
  }

  Py_DECREF(pickle_tmp_filename);
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
