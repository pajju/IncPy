/* 
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_MEMOIZE_REACHABILITY_H
#define Py_MEMOIZE_REACHABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"


// ORDER MATTERS for short-circuiting ... put the most commonly-occuring
// ones first ...
#define DEFINITELY_IMMUTABLE(obj) \
  ((obj == Py_None) ||            \
   PyString_CheckExact(obj) ||    \
   PyInt_CheckExact(obj) ||       \
   PyLong_CheckExact(obj) ||      \
   PyBool_Check(obj) ||           \
   PyComplex_CheckExact(obj) ||   \
   PyFloat_CheckExact(obj) ||     \
   PyUnicode_CheckExact(obj) ||   \
   PyFunction_Check(obj) ||       \
   PyCFunction_Check(obj) ||      \
   PyMethod_Check(obj) ||         \
   PyType_CheckExact(obj) ||      \
   PyClass_Check(obj) ||          \
   PyFile_CheckExact(obj))


PyObject* find_globally_reachable_obj_by_name(PyObject* varname_tuple,
                                              PyFrameObject* cur_frame);

PyObject* create_varname_tuple(PyObject* filename, PyObject* varname);
PyObject* extend_with_attrname(PyObject* parent, PyObject* attrname);
void update_global_container_weakref(PyObject* obj, PyObject* new_elt);
void update_arg_reachable_func_start_time(PyObject* parent, PyObject* child);
PyObject* get_global_container(PyObject* obj);

int contains_externally_aliased_mutable_obj(PyObject* obj, PyFrameObject* f);

// initialize in pg_initialize(), destroy in pg_finalize()
PyObject* global_containment_intern_cache;


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_REACHABILITY_H */

