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

// Object that contains the memo table entry, dependencies, and profiling
// metadata for one function (only some fields will be serialized to disk)
typedef struct {
  // these fields are serialized to disk
  // (for efficiency, these are NULL pointers if empty)

  // (we can't make this into a dict since not all argument values are
  // hashable as keys ... instead, we will make this a list of
  // dicts, where the keys are:
  //   "args" --> argument list
  //   "retval" --> return value, stored in a SINGLETON list
  //                (to facilitate mutation for COW optimization)
  //   "runtime_ms" --> how many milliseconds it took to run
  //   "stdout_buf" --> buffered stdout string (OPTIONAL!)
  //   "stderr_buf" --> buffered stderr string (OPTIONAL!)
  //
  // to support lazy-loading, this field is serialized to disk as:
  //   incpy-cache/XXX.memoized_vals.pickle
  //
  // don't access this field directly ...
  //   instead use get_memoized_vals_lst()
  PyObject* memoized_vals;            // List

  // code dependency 'object' (Dict) representing the saved state of
  // this function's code
  PyObject* self_code_dependency;


  // these fields are NOT serialized to disk

  PyObject* f_code; // PyCodeObject that contains the function's
                    // canonical name as pg_canonical_name
                    // (see GET_CANONICAL_NAME macro below)

  // specifies the last time that this FuncMemoInfo was checked for
  // dependencies by are_dependencies_satisfied(), used to prevent
  // infinite loops
  //
  // has the same units as 'num_executed_func_calls'
  unsigned int last_dep_check_func_call_time;

  // booleans
  char is_impure;    // is this function impure during THIS execution?
  char self_code_dependency_SAT; // is self_code_dependency satisfied for THIS execution?
  char memoized_vals_loaded; // have we attempted to load memoized_vals from disk yet?

} FuncMemoInfo;

#define GET_CANONICAL_NAME(fmi) ((PyCodeObject*)fmi->f_code)->pg_canonical_name

#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_FMI_H */

