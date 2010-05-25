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
  //
  //   "args" --> argument list
  //   "global_vars_read" --> dict mapping global vars to values (OPTIONAL)
  //
  //   "files_read" --> dict mapping files read to modtimes (OPTIONAL)
  //   "files_written" --> dict mapping files written to modtimes (OPTIONAL)
  //
  //   "retval" --> return value, stored in a SINGLETON list
  //                (to facilitate mutation for COW optimization)
  //   "stdout_buf" --> buffered stdout string (OPTIONAL)
  //   "stderr_buf" --> buffered stderr string (OPTIONAL)
  //
  //   "runtime_ms" --> how many milliseconds it took to run
  //
  // to support lazy-loading, this field is serialized to disk as:
  //   incpy-cache/XXX.memoized_vals.pickle
  //
  // don't access this field directly ...
  //   instead use get_memoized_vals_lst()
  PyObject* memoized_vals;            // List

  // all of these fields are serialized to disk as:
  //   incpy-cache/XXX.dependencies.pickle
  PyObject* code_dependencies;        // Dict


  // these fields are NOT serialized to disk

  PyObject* f_code; // PyCodeObject that contains the function's
                    // canonical name as pg_canonical_name
                    // (see GET_CANONICAL_NAME macro below)

  // booleans
  char is_impure;    // is this function impure during THIS execution?
  char all_code_deps_SAT; // are all code dependencies satisfied during THIS execution?
  char memoized_vals_loaded; // have we attempted to load memoized_vals from disk yet?

} FuncMemoInfo;

#define GET_CANONICAL_NAME(fmi) ((PyCodeObject*)fmi->f_code)->pg_canonical_name

#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_FMI_H */

