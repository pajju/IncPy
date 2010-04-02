/* Serializable code dependency 'class'
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_MEMOIZE_CODEDEP_H
#define Py_MEMOIZE_CODEDEP_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"


PyObject* CREATE_NEW_code_dependency(PyCodeObject* codeobj);
int code_dependency_EQ(PyObject* codedep1, PyObject* codedep2);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_CODEDEP_H */

