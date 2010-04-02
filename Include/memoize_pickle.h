/* Support code for pickling
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_MEMOIZE_PICKLE_H
#define Py_MEMOIZE_PICKLE_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"


// use *_CheckExact for speed ...

// ORDER MATTERS for short-circuiting ... put the most commonly-occuring
// ones first ...
#define DEFINITELY_PICKLABLE(obj) \
  ((obj == Py_None) || \
   PyString_CheckExact(obj) || \
   PyInt_CheckExact(obj) || \
   PyLong_CheckExact(obj) || \
   PyBool_Check(obj) || \
   PyComplex_CheckExact(obj) || \
   PyFloat_CheckExact(obj) || \
   PyUnicode_CheckExact(obj))


// TODO: In theory, each of these types could be traceable by replacing
// them with proxies, but I don't want to put in that work unless there
// is demand for it.  e.g., create code dependencies for function
// objects, and file dependencies for module objects, etc.
//
// ORDER MATTERS for short-circuiting ... put the most commonly-occuring
// ones first ...
#define DEFINITELY_NOT_PICKLABLE(val) \
  (PyModule_CheckExact(val) || \
   PyFunction_Check(val) || \
   PyCFunction_Check(val) || \
   PyMethod_Check(val) || \
   PyType_CheckExact(val) || \
   PyClass_Check(val) || \
   PyFile_CheckExact(val))


int is_picklable(PyObject* obj);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_PICKLE_H */

