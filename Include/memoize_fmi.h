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

// Object that contains the code dependencies and profiling
// metadata for one function
typedef struct {
  // Key: canonical name of function called by this one
  // Value: code dependency 'object' (see Python/memoize_codedep.c)
  PyObject* code_dependencies; // Dict

  PyObject* f_code; // PyCodeObject that contains the function's
                    // canonical name as pg_canonical_name
                    // (see GET_CANONICAL_NAME macro below)

  // PyString representing the relative path to the sub-directory where
  // this function's cache entries reside
  PyObject* cache_subdirectory_path;

  // booleans
  char is_impure;    // is this function impure during THIS execution?

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
  // (small value between 0 and 255, to conserve struct space)
  unsigned char num_fast_calls_with_no_memoized_vals;

} FuncMemoInfo;

#define GET_CANONICAL_NAME(fmi) ((PyCodeObject*)fmi->f_code)->pg_canonical_name


PyObject* on_disk_cache_GET(FuncMemoInfo* fmi, PyObject* hash_key);
PyObject* on_disk_cache_PUT(FuncMemoInfo* fmi, PyObject* hash_key, PyObject* contents);
void on_disk_cache_DEL(FuncMemoInfo* fmi, PyObject* hash_key);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_FMI_H */

