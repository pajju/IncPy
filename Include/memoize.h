/* The rest of the interpreter should only call functions exported by
   this header file.  These functions are the link between IncPy and the
   rest of the interpreter.

   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.

   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/


#ifndef Py_MEMOIZE_H
#define Py_MEMOIZE_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"
#include "frameobject.h"


#define PYPRINT(obj) do {PyObject_Print(obj, stdout, 0); printf("\n");} while(0)


PyFrameObject* top_frame;


// initialize in pg_initialize(), destroy in pg_finalize()
PyObject* all_func_memo_info_dict;
PyObject* func_name_to_code_dependency;

PyObject* deepcopy_func;
PyObject* cPickle_dumpstr_func;
PyObject* cPickle_load_func;
PyObject* pickle_filenames;

int obj_equals(PyObject* obj1, PyObject* obj2);

void add_new_code_dep(PyCodeObject* cod);

// hook from PyCode_New()
void pg_init_canonical_name_and_ignore(PyCodeObject* co);


void pg_initialize(void);
void pg_finalize(void);

PyObject* pg_enter_frame(PyFrameObject* f);
void pg_exit_frame(PyFrameObject* f, PyObject* retval);

// handler for LOAD_GLOBAL(varname) --> value
void pg_LOAD_GLOBAL_event(PyObject *varname, PyObject *value);
// handler for LOAD(object.attrname) --> value
void pg_GetAttr_event(PyObject *object, PyObject *attrname, PyObject *value);
// handler for BINARY_SUBSCR opcode: ret = obj[ind]
void pg_BINARY_SUBSCR_event(PyObject* obj, PyObject* ind, PyObject* res);

// handler for STORE_GLOBAL(varname) or DELETE_GLOBA(varname)
void pg_STORE_DEL_GLOBAL_event(PyObject *varname);

// called whenever object is ABOUT TO BE mutated by either storing or
// deleting one of its attributes (e.g., in an instance) or items (e.g.,
// in a sequence)
void pg_about_to_MUTATE_event(PyObject *object);

// handler for PyFunction_New()
void pg_CREATE_FUNCTION_event(PyObject* func);

// handler for BUILD_CLASS opcode
void pg_BUILD_CLASS_event(PyObject* name, PyObject* methods_dict);

void pg_FILE_OPEN_event(PyFileObject* fobj);
void pg_FILE_CLOSE_event(PyFileObject* fobj);

void pg_FILE_READ_event(PyFileObject* fobj);


// when you're calling a method implemented in C with the name func_name
// and the self object 'self' ... e.g.,
// lst = [1,2,3]
// lst.append(4)
// (here, func_name is "append" and self is the list object [1,2,3])
void pg_about_to_CALL_C_METHOD_WITH_SELF_event(char* func_name, PyObject* self);


// handlers for file write operations as defined in Objects/fileobject.c:
void pg_intercept_PyFile_WriteString(const char *s, PyObject *f);
void pg_intercept_PyFile_WriteObject(PyObject *v, PyObject *f, int flags);
void pg_intercept_PyFile_SoftSpace(PyObject *f, int newflag);
void pg_intercept_file_write(PyFileObject *f, PyObject *args);
void pg_intercept_file_writelines(PyFileObject *f, PyObject *args);
void pg_intercept_file_writelines(PyFileObject *f, PyObject *seq);
void pg_intercept_file_truncate(PyFileObject *f, PyObject *args);

// intercept in Objects/codeobject.c:PyCode_New()
int pg_ignore_code(PyCodeObject* co);
PyObject* pg_create_canonical_code_name(PyCodeObject* co);

#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_H */

