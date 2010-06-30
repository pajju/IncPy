/* The FuncMemoInfo 'class'
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_MEMOIZE_FMI_H
#define Py_MEMOIZE_FMI_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"

// Object that contains the memo table, dependencies, and profiling
// metadata for one function (only some fields will be serialized to disk)
typedef struct {
  // at the end of execution, these fields are serialized to disk as:
  //   incpy-cache/XXX.dependencies.pickle
  PyObject* code_dependencies; // Dict

  // these fields below are NOT serialized to disk

  PyObject* f_code; // PyCodeObject that contains the function's
                    // canonical name as pg_canonical_name
                    // (see GET_CANONICAL_NAME macro below)

  // booleans
  char is_impure;    // is this function impure during THIS execution?
  char all_code_deps_SAT; // are all code dependencies satisfied during THIS execution?

  // should we not even bother memoizing this function? (but we still
  // need to track its dependencies) ... only relevant when
  // ENABLE_IGNORE_FUNC_THRESHOLD_OPTIMIZATION on
  char likely_nothing_to_memoize;

  // if the incpy-cache/<hash of function name>.cache/ sub-directory doesn't
  // exist, then the on-disk cache for this function is currently empty,
  // so no need to check it
  char on_disk_cache_empty;


  // how many times has this function been executed and terminated
  // 'quickly' with the memoized_vals field as NULL?  only relevant when
  // ENABLE_IGNORE_FUNC_THRESHOLD_OPTIMIZATION is on
  unsigned int num_fast_calls_with_no_memoized_vals;

} FuncMemoInfo;

#define GET_CANONICAL_NAME(fmi) ((PyCodeObject*)fmi->f_code)->pg_canonical_name


PyObject* on_disk_cache_GET(FuncMemoInfo* fmi, PyObject* hash_key);
PyObject* on_disk_cache_PUT(FuncMemoInfo* fmi, PyObject* hash_key, PyObject* contents);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_FMI_H */

