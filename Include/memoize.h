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


// initialize in pg_initialize(), destroy in pg_finalize()
PyObject* all_func_memo_info_dict;
PyObject* func_name_to_code_dependency;

PyObject* cPickle_dumpstr_func;
PyObject* cPickle_load_func;
PyObject* pickle_filenames;

PyObject* deepcopy(PyObject* obj);

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


/* check for word size

   stolen from Valgrind: valgrind-3.5.0/VEX/pub/libvex_basictypes.h */

/* The following 4 work OK for Linux. */
#if defined(__x86_64__)
#   define HOST_WORDSIZE 8
#elif defined(__i386__)
#   define HOST_WORDSIZE 4
#elif defined(__powerpc__) && defined(__powerpc64__)
#   define HOST_WORDSIZE 8
#elif defined(__powerpc__) && !defined(__powerpc64__)
#   define HOST_WORDSIZE 4

#elif defined(_AIX) && !defined(__64BIT__)
#   define HOST_WORDSIZE 4
#elif defined(_AIX) && defined(__64BIT__)
#   define HOST_WORDSIZE 8

#else
#   error "IncPy compilation error: Can't establish the host architecture"
#endif

#if HOST_WORDSIZE == 8
#   define HOST_IS_64BIT
#endif

// check their sizes in pg_initialize():
typedef unsigned short          UInt16;
typedef unsigned int            UInt32;
typedef unsigned long long int  UInt64;

#define METADATA_MAP_SIZE 65536 // 16 bits
#define METADATA_MAP_MASK (METADATA_MAP_SIZE-1)

/* efficient multi-level mapping of PyObject addresses to metadata
   (inspired by Valgrind Memcheck's multi-level shadow memory
    implementation: http://valgrind.org/docs/shadow-memory2007.pdf)

   We do this 'shadowing' rather than directly augmenting the PyObject
   struct so that we can maintain BINARY COMPATIBILITY with existing
   libraries containing compiled extension code (e.g., numpy, scipy).
   Pre-compiled versions of these libraries are hard-coded with the
   default PyObject struct layout, so if we change PyObject by adding
   fields to it, then we will need to re-compile those libraries,
   which is a big pain! */
typedef struct {
  struct {
    unsigned int creation_time; // measured in number of elapsed function calls
    // WEAK REFERENCE - This should only be set for MUTABLE values
    // (see update_global_container for more details on why)
    PyObject* global_container_weakref;
  } contents[METADATA_MAP_SIZE];
} obj_metadata_map;

// allocate and zero out in pg_initialize():
#ifdef HOST_IS_64BIT
// 64-bit architecture

// TODO: implement me!!!

#else
// 32-bit architecture

// initialize to an array of METADATA_MAP_SIZE:
obj_metadata_map** level_1_map;
#endif

void set_global_container(PyObject* obj, PyObject* global_container);
PyObject* get_global_container(PyObject* obj);

void set_creation_time(PyObject* obj, unsigned int creation_time);
unsigned int get_creation_time(PyObject* obj);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_H */

