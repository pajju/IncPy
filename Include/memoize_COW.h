/* Copy-on-write optimization
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_MEMOIZE_COW_H
#define Py_MEMOIZE_COW_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"

// initialize in pg_initialize(), destroy in pg_finalize()
PyObject* cow_containment_dict;
PyObject* cow_traced_addresses_set;

void cow_containment_dict_ADD(PyObject* obj);
void check_COW_mutation(PyObject* obj);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_COW_H */

