/* Main public-facing functions
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#include "memoize.h"
#include "memoize_fmi.h"
#include "memoize_COW.h"
#include "memoize_logging.h"
#include "memoize_profiling.h"
#include "memoize_codedep.h"
#include "memoize_pickle.h"
#include "memoize_reachability.h"

#include "dictobject.h"
#include "import.h"

#include "cStringIO.h"

#include <time.h>
#include <sys/stat.h>
#include <string.h>

/* blindly trust the memoized results of previous executions, even if
   your code has changed (unsound in general, but effective in cases
   where the user knows what he/she is doing)

   activated by '-T' command-line option (see Modules/main.c)

Oftentimes what happens is that you're writing a function to process
individual records in a dataset (ranging from 1 to N).  It runs fine
until it crashes on one record number i somewhere in the middle of your
dataset since that record contained some unexpected data.  With IncPy,
the results from processing records 1 through (i - 1) have been memoized
to disk, so if you re-run your script, you can re-use those results.
But since your code crashed, you probably want to modify it before
re-running.  However, since your code has been modified, IncPy must
invalidate the cache entries for processing records 1 through (i - 1),
which gives you NO time savings :(

However, with trust_prev_memoized_results activated, IncPy simply trusts
the previously-cached results of a function, even if its code changes.
While this is unsafe in general, I think that there are a large class of
edits for which this is actually safe.

ewencp also added that even if the results from previously-processed
records are WRONG after the code change, this -T mode can still be very
useful because he simply wants to skip the computation of previous
results and get on with munging of the not-yet-processed records (so
that he can hash out any remaining bugs in his code) ... at the VERY
END, after he's knocked off all the bugs, he will then do a FULL run to
compute the FINAL definitive results.

 */
int trust_prev_memoized_results = 0;


// Note that ALL of these options #ifdef flags should be only limited to
// THIS FILE, since if you use them in other files, they will likely not
// be activated at the time those files are compiled ...


// turn this on to COMPLETELY disable the functionality of this module
// (good for getting Python to just compile after a 'make clean')
//#define DISABLE_MEMOIZE


// start this at 0 and then only set to 1 after pg_initialize().
// then set back to 0 whenever you're executing within
// memoization-related code, since we don't want to trace data structure
// mutations or Python function calls made within memoization-related code
int pg_activated = 0;


/* IncPy coding conventions:

- Code from the rest of the Python interpreter should only call
functions in THIS file (made public through Include/memoize.h),
not functions in ANY OTHER memoize_* files!

- Every public-facing function (those that are called by the rest of the
Python interpreter) must begin with a MEMOIZE_PUBLIC_START() macro call,
for void functions, or MEMOIZE_PUBLIC_START_RETNULL() for functions that
return a pointer

- ALL exits to every public-facing function must end with a
MEMOIZE_PUBLIC_END() macro call (don't forget error paths!)

- By convention, all public-facing functions are prefixed with 'pg_';
all other functions are private (should only be called by
memoization-related code, not by the rest of the interpreter)

- The main invariant we want to ensure is that as soon as execution
enters memoization code, pg_activated should be set to 0


Wrapping public-facing functions in MEMOIZE_PUBLIC_START() and
MEMOIZE_PUBLIC_END() is VERY IMPORTANT for performance reasons, since we
often use built-in Python data structures and call Python functions like
(cPickle.load) in our memoization algorithms.  When those data
structures are read or written to (or those functions are called), we
should NOT be delegating AGAIN to our memoization tracking code (e.g.,
pg_about_to_MUTATE_event()).  We should only be tracking the target
program, not our memoization support code.

*/
#ifdef DISABLE_MEMOIZE
  #define MEMOIZE_PUBLIC_START() return;
  #define MEMOIZE_PUBLIC_START_RETNULL() return NULL;
#else
  #define MEMOIZE_PUBLIC_START() \
    if (!pg_activated) return; \
    pg_activated = 0;
  #define MEMOIZE_PUBLIC_START_RETNULL() \
    if (!pg_activated) return NULL; \
    pg_activated = 0;
#endif

#define MEMOIZE_PUBLIC_END() \
  assert(!pg_activated); \
  pg_activated = 1;


// enable copy-on-write optimization
#define ENABLE_COW


#ifdef ENABLE_DEBUG_LOGGING // defined in "memoize_logging.h"

/* This is the debug log file that's initialized in pg_initialize() to
   open a file called 'memoize.log' */
FILE* debug_log_file = NULL;
#endif


/* the user-facing log is an APPEND-only log that's meant to be
   low-traffic and report only important events relating to memoization

   It's initialized in pg_initialize() to open a file called
   $HOME/incpy.aggregate.log (in the user's home directory)
*/
static FILE* user_aggregate_log_file = NULL;

/* same as user_log_file, except that it's write-only and only keeps the
   log for the most recent execution

   It's initialized in pg_initialize() to open a file called incpy.log

*/
static FILE* user_log_file = NULL;

#define USER_LOG_PRINTF(...) \
  do { \
    fprintf(user_log_file, __VA_ARGS__); \
    fprintf(user_aggregate_log_file, __VA_ARGS__); \
} while(0)


// lazily initialize a field like stdout_cStringIO and stderr_cStringIO
#define LAZY_INIT_STRINGIO_FIELD(x) \
  do { \
    if (!x) { \
      (x) = PycStringIO->NewOutput(128); \
      assert(x); \
    } \
  } while (0)

// lazily initialize set s and add elt to it
#define LAZY_INIT_SET_ADD(s, elt) \
  do { \
    if (!(s)) { \
      (s) = PySet_New(NULL); \
    } \
    PySet_Add((s), (elt)); \
  } while (0)


// References to Python standard library functions:

static PyObject* deepcopy_func = NULL;        // copy.deepcopy
PyObject* cPickle_load_func = NULL;           // cPickle.load
PyObject* cPickle_dumpstr_func = NULL;        // cPickle.dumps

static PyObject* cPickle_dump_func = NULL;    // cPickle.dump
static PyObject* hashlib_md5_func = NULL;     // hashlib.md5

static PyObject* abspath_func = NULL; // os.path.abspath

PyObject* ignore_str = NULL;

// lazily import this if necessary
static PyObject* numpy_module = NULL;


// forward declarations:
static void mark_impure(PyFrameObject* f, char* why);
static void mark_entire_stack_impure(char* why);
static void deepcopy_and_add_global_read(PyObject* varname,
                                         PyObject* value,
                                         PyObject* output_dict);
static void add_file_dependency(PyObject* filename, PyObject* output_dict);

// to shut gcc up ...
extern time_t PyOS_GetLastModificationTime(char *, FILE *);


// our notion of 'time' within an execution, measured by number of
// elapsed function calls:
unsigned int num_executed_func_calls = 0;

// from memoize_fmi.c
extern FuncMemoInfo* NEW_func_memo_info(PyCodeObject* cod);
extern void DELETE_func_memo_info(FuncMemoInfo* fmi);
extern void clear_cache_and_mark_pure(FuncMemoInfo* func_memo_info);
extern FuncMemoInfo* get_func_memo_info_from_cod(PyCodeObject* cod);
extern PyObject* serialize_func_memo_info_dependencies(FuncMemoInfo* func_memo_info);
extern PyObject* get_memoized_vals_lst(FuncMemoInfo* fmi);


// set time limit to something smaller for debug mode, so that my
// regression tests can run faster
#if defined(Py_DEBUG)
unsigned int memoize_time_limit_ms = 100;
#else
unsigned int memoize_time_limit_ms = 1000;
#endif // Py_DEBUG


/* Dict where each key is a canonical name and each value is a PyLong
   that represents a POINTER to the corresponding FuncMemoInfo struct
   (we can't directly store FuncMemoInfo structs since they're not a
   subtype of PyObject).
 
   This is the in-memory representation of FuncMemoInfo objects.  They
   are stored on-disk as pickle files. */
PyObject* all_func_memo_info_dict = NULL;


/* A DICT that contains all functions mapped to their respective
   code_dependency objects (add new entries using add_new_code_dep())

   Key: canonical name
   Value: A code_dependency 'object' (dict) representing a picklable 
          version of the function's PyCodeObject */
PyObject* func_name_to_code_dependency = NULL;

/* A DICT that maps canonical name to the ACTUAL PyCodeObject with that
   canonical name (not to be confused with func_name_to_code_dependency,
   which contains a picklable version of the PyCodeObject).

   (add new entries using add_new_code_dep())
 
   This should be kept in-sync with func_name_to_code_dependency. */
static PyObject* func_name_to_code_object = NULL;


// A list of paths to ignore for the purposes of tracking code
// dependencies and impure actions (specified in $HOME/incpy.config)
//
// Each element should be an absolute path of an existent file or directory
static PyObject* ignore_paths_lst = NULL;


// Super-simple trie implementation for doing fast string matching:

typedef struct _trie {
  struct _trie* children[128]; // we support ASCII characters 0 to 127
  int elt_is_present; // 1 if there is an element present here
} Trie;


static Trie* TrieCalloc(void) {
  Trie* new_trie = PyMem_New(Trie, 1);
  // VERY important to blank out all elements
  memset(new_trie, 0, sizeof(*new_trie));
  return new_trie;
}

static void TrieFree(Trie* t) {
  // free all your children before freeing yourself
  unsigned char i;
  for (i = 0; i < 128; i++) {
    if (t->children[i]) {
      TrieFree(t->children[i]);
    }
  }

  PyMem_Del(t);
}

static void TrieInsert(Trie* t, char* ascii_string) {
  while (*ascii_string != '\0') {
    unsigned char idx = (unsigned char)*ascii_string;
    assert(idx < 128); // we don't support extended ASCII characters
    if (!t->children[idx]) {
      t->children[idx] = TrieCalloc();
    }
    t = t->children[idx];
    ascii_string++;
  }

  t->elt_is_present = 1;
}

static int TrieContains(Trie* t, char* ascii_string) {
  while (*ascii_string != '\0') {
    unsigned char idx = (unsigned char)*ascii_string;
    t = t->children[idx];
    if (!t) {
      return 0; // early termination, no match!
    }
    ascii_string++;
  }

  return t->elt_is_present;
}

// trie containing the names of C methods that mutate their 'self' arg
// --- initialize in pg_initialize():
static Trie* self_mutator_c_methods = NULL;
static Trie* definitely_impure_funcs = NULL;

static void init_self_mutator_c_methods(void);
static void init_definitely_impure_funcs(void);


// efficient multi-level mapping of PyObject addresses to metadata

// sanity check
int obj_metadata_array_is_empty(obj_metadata_array* arr) {
  int i;
  for (i = 0; i < METADATA_MAP_SIZE; i++) {
    if (arr->elts[i].allocated) {
      return 0;
    }
  }
  return 1;
}


#ifdef HOST_IS_64BIT
// 64-bit architecture

// ugh, unfortunately the straightforward implementation for 64-bit is
// much more kludgy and probably slower than the 32-bit implementation

obj_metadata**** level_1_map = NULL;

void set_global_container(PyObject* obj, PyObject* global_container) {
  if (!level_1_map) {
    return;
  }

  assert(global_container);

  UInt16 level_1_addr = (((UInt64)obj) >> 48) & METADATA_MAP_MASK;
  UInt16 level_2_addr = (((UInt64)obj) >> 32) & METADATA_MAP_MASK;
  UInt16 level_3_addr = (((UInt64)obj) >> 16) & METADATA_MAP_MASK;
  UInt16 level_4_addr = ((UInt64)obj) & METADATA_MAP_MASK;

  obj_metadata*** level_2_map = level_1_map[level_1_addr];
  if (!level_2_map) {
    level_2_map = level_1_map[level_1_addr] = PyMem_New(obj_metadata**, METADATA_MAP_SIZE);
    memset(level_2_map, 0, sizeof(obj_metadata**) * METADATA_MAP_SIZE);
  }

  obj_metadata** level_3_map = level_2_map[level_2_addr];
  if (!level_3_map) {
    level_3_map = level_2_map[level_2_addr] = PyMem_New(obj_metadata*, METADATA_MAP_SIZE);
    memset(level_3_map, 0, sizeof(obj_metadata*) * METADATA_MAP_SIZE);
  }

  obj_metadata* level_4_map = level_3_map[level_3_addr];
  if (!level_4_map) {
    level_4_map = level_3_map[level_3_addr] = PyMem_New(obj_metadata, METADATA_MAP_SIZE);
    memset(level_4_map, 0, sizeof(obj_metadata) * METADATA_MAP_SIZE);
  }

  level_4_map[level_4_addr].global_container_weakref = global_container;
}

PyObject* get_global_container(PyObject* obj) {
  if (!level_1_map) {
    return NULL;
  }

  UInt16 level_1_addr = (((UInt64)obj) >> 48) & METADATA_MAP_MASK;
  if (!level_1_map[level_1_addr]) {
    return NULL;
  }

  UInt16 level_2_addr = (((UInt64)obj) >> 32) & METADATA_MAP_MASK;
  if(!level_1_map[level_1_addr][level_2_addr]) {
    return NULL;
  }

  UInt16 level_3_addr = (((UInt64)obj) >> 16) & METADATA_MAP_MASK;
  if (!level_1_map[level_1_addr][level_2_addr][level_3_addr]) {
    return NULL;
  }

  UInt16 level_4_addr = ((UInt64)obj) & METADATA_MAP_MASK;
  return level_1_map[level_1_addr][level_2_addr][level_3_addr][level_4_addr].global_container_weakref;
}

void set_creation_time(PyObject* obj, unsigned int creation_time) {
  if (!level_1_map) {
    return;
  }

  // fast-path ... don't do anything when creation_time is 0!
  if (creation_time == 0) {
    return;
  }

  UInt16 level_1_addr = (((UInt64)obj) >> 48) & METADATA_MAP_MASK;
  UInt16 level_2_addr = (((UInt64)obj) >> 32) & METADATA_MAP_MASK;
  UInt16 level_3_addr = (((UInt64)obj) >> 16) & METADATA_MAP_MASK;
  UInt16 level_4_addr = ((UInt64)obj) & METADATA_MAP_MASK;

  //printf("set_creation_time: %p (%p %p %p %p)\n",
  //       (void*)obj, (void*)level_1_addr, (void*)level_2_addr, (void*)level_3_addr, (void*)level_4_addr);

  obj_metadata*** level_2_map = level_1_map[level_1_addr];
  if (!level_2_map) {
    level_2_map = level_1_map[level_1_addr] = PyMem_New(obj_metadata**, METADATA_MAP_SIZE);
    memset(level_2_map, 0, sizeof(obj_metadata**) * METADATA_MAP_SIZE);
    //printf("  NEW level_2_map %p (size=%d)\n", (void*)level_1_addr,
    //       sizeof(obj_metadata**) * METADATA_MAP_SIZE);
  }

  obj_metadata** level_3_map = level_2_map[level_2_addr];
  if (!level_3_map) {
    level_3_map = level_2_map[level_2_addr] = PyMem_New(obj_metadata*, METADATA_MAP_SIZE);
    memset(level_3_map, 0, sizeof(obj_metadata*) * METADATA_MAP_SIZE);
    //printf("  NEW level_3_map %p (size=%d)\n", (void*)level_2_addr,
    //       sizeof(obj_metadata*) * METADATA_MAP_SIZE);
  }

  obj_metadata* level_4_map = level_3_map[level_3_addr];
  if (!level_4_map) {
    level_4_map = level_3_map[level_3_addr] = PyMem_New(obj_metadata, METADATA_MAP_SIZE);
    memset(level_4_map, 0, sizeof(obj_metadata) * METADATA_MAP_SIZE);
    //printf("  NEW level_4_map %p (size=%d)\n", (void*)level_3_addr,
    //       sizeof(obj_metadata) * METADATA_MAP_SIZE);
  }

  level_4_map[level_4_addr].creation_time = creation_time;

  // sanity check - comment out for speed:
  //assert(get_creation_time(obj) == creation_time);
}

// return 0 if not found (earliest possible creation time)
unsigned int get_creation_time(PyObject* obj) {
  if (!level_1_map) {
    return 0;
  }

  UInt16 level_1_addr = (((UInt64)obj) >> 48) & METADATA_MAP_MASK;
  if (!level_1_map[level_1_addr]) {
    return 0;
  }

  UInt16 level_2_addr = (((UInt64)obj) >> 32) & METADATA_MAP_MASK;
  if(!level_1_map[level_1_addr][level_2_addr]) {
    return 0;
  }

  UInt16 level_3_addr = (((UInt64)obj) >> 16) & METADATA_MAP_MASK;
  if (!level_1_map[level_1_addr][level_2_addr][level_3_addr]) {
    return 0;
  }

  UInt16 level_4_addr = ((UInt64)obj) & METADATA_MAP_MASK;
  return level_1_map[level_1_addr][level_2_addr][level_3_addr][level_4_addr].creation_time;
}


#else
// 32-bit architecture

obj_metadata_array** level_1_map = NULL;

void set_global_container(PyObject* obj, PyObject* global_container) {
  if (!level_1_map) {
    return;
  }

  assert(global_container);

  UInt16 msb16 = (((UInt32)obj) >> 16) & METADATA_MAP_MASK;
  UInt16 lsb16 = ((UInt32)obj) & METADATA_MAP_MASK;

  obj_metadata_array* level_2_map = level_1_map[msb16];

  if (!level_2_map) {
    level_2_map = level_1_map[msb16] = PyMem_New(obj_metadata_array, 1);
    memset(level_2_map, 0, sizeof(obj_metadata_array));
    //printf("  sgc NEW level_2_map %p (size=%d)\n", (void*)msb16, sizeof(obj_metadata_array));
  }

  if (!level_2_map->elts[lsb16].allocated) {
    level_2_map->elts[lsb16].allocated = 1;
    level_2_map->num_allocated_elts++;
  }

  level_2_map->elts[lsb16].global_container_weakref = global_container;
}

PyObject* get_global_container(PyObject* obj) {
  if (!level_1_map) {
    return NULL;
  }

  UInt16 msb16 = (((UInt32)obj) >> 16) & METADATA_MAP_MASK;
  UInt16 lsb16 = ((UInt32)obj) & METADATA_MAP_MASK;

  if (!level_1_map[msb16]) {
    return NULL;
  }
  else {
    return level_1_map[msb16]->elts[lsb16].global_container_weakref;
  }
}

void set_creation_time(PyObject* obj, unsigned int creation_time) {
  if (!level_1_map) {
    return;
  }

  // fast-path ... don't do anything when creation_time is 0!
  if (creation_time == 0) {
    return;
  }

  UInt16 msb16 = (((UInt32)obj) >> 16) & METADATA_MAP_MASK;
  UInt16 lsb16 = ((UInt32)obj) & METADATA_MAP_MASK;

  //printf("set_creation_time: %p (%p %p)\n", (void*)obj, (void*)msb16, (void*)lsb16);

  obj_metadata_array* level_2_map = level_1_map[msb16];

  if (!level_2_map) {
    level_2_map = level_1_map[msb16] = PyMem_New(obj_metadata_array, 1);
    memset(level_2_map, 0, sizeof(obj_metadata_array));
    //printf("  sct NEW level_2_map %p (size=%d)\n", (void*)msb16, sizeof(obj_metadata_array));
  }

  if (!level_2_map->elts[lsb16].allocated) {
    level_2_map->elts[lsb16].allocated = 1;
    level_2_map->num_allocated_elts++;
  }

  level_2_map->elts[lsb16].creation_time = creation_time;

  // sanity check - comment out for speed:
  assert(get_creation_time(obj) == creation_time);
}

// return 0 if not found (earliest possible creation time)
unsigned int get_creation_time(PyObject* obj) {
  if (!level_1_map) {
    return 0;
  }

  UInt16 msb16 = (((UInt32)obj) >> 16) & METADATA_MAP_MASK;
  UInt16 lsb16 = ((UInt32)obj) & METADATA_MAP_MASK;

  if (!level_1_map[msb16]) {
    return 0;
  }
  else {
    return level_1_map[msb16]->elts[lsb16].creation_time;
  }
}

// called whenever an object is deallocated
void pg_dealloc_object(PyObject* obj) {
  if (!level_1_map) {
    return;
  }

  UInt16 msb16 = (((UInt32)obj) >> 16) & METADATA_MAP_MASK;
  UInt16 lsb16 = ((UInt32)obj) & METADATA_MAP_MASK;

  //printf("DEALLOC %p %p\n", (void*)msb16, (void*)lsb16);

  obj_metadata_array* level_2_map = level_1_map[msb16];

  if (level_2_map &&
      level_2_map->elts[lsb16].allocated) {
    // deallocate shadow entry:
    memset(&level_2_map->elts[lsb16], 0, sizeof(obj_metadata));
    assert(level_2_map->num_allocated_elts > 0);
    level_2_map->num_allocated_elts--;

    if (level_2_map->num_allocated_elts == 0) {
      // can take a long time to run, only activate in debug mode
      assert(obj_metadata_array_is_empty(level_2_map));

      printf("  FREE %p\n", (void*)msb16);
      PyMem_Del(level_2_map);
      level_1_map[msb16] = NULL; // REALLY important!!!
    }
  }
}

#endif

/* proxy objects - for objects that can't be pickled, we can instead
   create picklable proxies in their place */

// ugh, ugly #include dependency, but I don't see any way of avoiding it :(
#include "Modules/_sqlite/cursor.h"
#include "Modules/_sqlite/connection.h"

// returns NULL if we don't need to create a proxy for the object
static PyObject* create_proxy_object(PyObject* obj) {
  // for files, create a tuple: ('FileProxy', <filename>)
  if (PyFile_Check(obj)) {
    PyFileObject* f = (PyFileObject*)obj;

    PyObject* proxy_tag = PyString_FromString("FileProxy");
    PyObject* ret = PyTuple_Pack(2, proxy_tag, f->f_name);
    Py_DECREF(proxy_tag);

    return ret;
  }
  // for functions, create a tuple: ('FunctionProxy', <canonical name>)
  else if (PyFunction_Check(obj)) {
    PyFunctionObject* f = (PyFunctionObject*)obj;
    PyCodeObject* cod = (PyCodeObject*)f->func_code;
    if (cod->pg_canonical_name) {
      PyObject* proxy_tag = PyString_FromString("FunctionProxy");
      PyObject* ret = PyTuple_Pack(2, proxy_tag, cod->pg_canonical_name);
      Py_DECREF(proxy_tag);

      return ret;
    }
    // if you don't have a canonical name, then punt ...
    else {
      return NULL;
    }
  }
  else {
    const char* obj_typename = Py_TYPE(obj)->tp_name;

    // for sqlite3 cursor, create a tuple: ('Sqlite3CursorProxy', <db filename>)
    if (strcmp(obj_typename, "sqlite3.Cursor") == 0) {
      pysqlite_Cursor* cur = (pysqlite_Cursor*)obj;
      pysqlite_Connection* conn = cur->connection;

      if (conn->db_file_handle->f_name) {
        PyObject* proxy_tag = PyString_FromString("Sqlite3CursorProxy");
        PyObject* ret = PyTuple_Pack(2, proxy_tag, conn->db_file_handle->f_name);
        Py_DECREF(proxy_tag);
        return ret;
      }
    }
  }

  return NULL;
}



// make a deep copy of obj and return it as a new reference
// (returns NULL on error)
PyObject* deepcopy(PyObject* obj) {
  assert(deepcopy_func);
  PyObject* ret = PyObject_CallFunctionObjArgs(deepcopy_func, obj, NULL);

  // if we failed, then try to see if we can find a copy() method,
  // and if so, try to use that instead.  (e.g., NumPy arrays and
  // matrices have their own copy methods)
  //
  // hmmm, we seem to do fine without this for now ... put it back in when we need it:
  /*
  if (!ret && PyObject_HasAttrString(obj, "copy")) {
    assert(PyErr_Occurred());
    PyErr_Clear();
    ret = PyObject_CallMethod(obj, "copy", NULL);
  }
  */

  return ret;
}


/* detect whether elt implements a non-identity-based comparison (e.g.,
   using __eq__ or __cmp__ methods); if not, then copies of elt loaded
   from disk will be a different object than the one in memory, so '=='
   will ALWAYS FAIL, even if they are semantically equal */
static int has_comparison_method(PyObject* elt) {
  // this is sort of abusing this macro, but all of these primitive
  // picklable types should pass with flying colors, even if they don't
  // explicitly implement a comparison method
  if (DEFINITELY_PICKLABLE(elt)) {
    return 1;
  }
  // instance objects always have tp_compare and tp_richcompare
  // methods, so we need to check for __eq__
  else if (PyInstance_Check(elt)) {
    return PyObject_HasAttrString(elt, "__eq__");
  }
  /* make an exception for compiled regular expression pattern objects,
     since although they don't implement comparison functions, they do
     some sort of interning that allows them to be compared properly
     using '==' */
  else if (strcmp(Py_TYPE(elt)->tp_name, "_sre.SRE_Pattern") == 0) {
    return 1;
  }
  else {
    return (Py_TYPE(elt)->tp_compare || Py_TYPE(elt)->tp_richcompare);
  }
}


// returns 1 iff "obj1 == obj2" in Python-world
// (can be SLOW when comparing large objects)
int obj_equals(PyObject* obj1, PyObject* obj2) {
  // first try the regular generic '==' comparison function:

  // we want to use this function because it implements "obj1 == obj2"
  // spec for PyObject_RichCompareBool from Objects/object.c:
  /* Return -1 if error; 1 if v op w; 0 if not (v op w). */
  int cmp_result = PyObject_RichCompareBool(obj1, obj2, Py_EQ);

  if (cmp_result < 0) {
    assert(PyErr_Occurred());

    // if a regular comparison gives an error, then try some
    // special-purpose comparison functions ...

    const char* obj1_typename = Py_TYPE(obj1)->tp_name;
    const char* obj2_typename = Py_TYPE(obj2)->tp_name;

    // use numpy.allclose(obj1, obj2) to compare NumPy arrays and
    // matrices, since '==' doesn't return a single boolean value
    //
    // TODO: use a trie if this seems too slow ...
    if ((strcmp(obj1_typename, "numpy.ndarray") == 0) ||
        (strcmp(obj1_typename, "matrix") == 0) ||
        (strcmp(obj1_typename, "MaskedArray") == 0) ||
        (strcmp(obj2_typename, "numpy.ndarray") == 0) ||
        (strcmp(obj2_typename, "matrix") == 0) ||
        (strcmp(obj2_typename, "MaskedArray") == 0)) {
      PyErr_Clear(); // forget the error, no worries :)

      // lazy initialize
      if (!numpy_module) {
        numpy_module = PyImport_ImportModule("numpy");
        assert(numpy_module);
      }

      PyObject* allclose_func = PyObject_GetAttrString(numpy_module, "allclose");
      assert(allclose_func);

      PyObject* res_bool = PyObject_CallFunctionObjArgs(allclose_func, obj1, obj2, NULL);
      Py_DECREF(allclose_func);

      if (res_bool) {
        int ret = PyObject_IsTrue(res_bool);
        Py_DECREF(res_bool);
        return ret;
      }
      else {
        // hide the failure and simply return 0 ...
        assert(PyErr_Occurred());
        PyErr_Clear();
        PG_LOG_PRINTF("dict(event='WARNING', what='Error in obj_equals', obj1_type='%s', obj2_type='%s')\n",
                      obj1_typename, obj2_typename);
        return 0;
      }
    }
    else {
      // hide the failure and simply return 0 ...
      assert(PyErr_Occurred());
      PyErr_Clear();
      PG_LOG_PRINTF("dict(event='WARNING', what='Error in obj_equals', obj1_type='%s', obj2_type='%s')\n",
                    obj1_typename, obj2_typename);
      return 0;
    }
  }

  return cmp_result;
}


// returns 1 if the absolute path represented by the PyString s
// appears as a PREFIX of some element in ignore_paths_lst
//
// TODO: we can use a trie to track prefixes if this seems slow in
// practice, but I won't refactor until it seems necessary
// (besides, if there are only a few entries in ignore_paths_lst,
// then a linear search + strncmp might not be so slow)
static int prefix_in_ignore_paths_lst(PyObject* s) {
  PyObject* path = NULL;

  // convert s to an ABSOLUTE PATH if possible
  // (otherwise, just use s as-is)
  if (abspath_func) {
    path = PyObject_CallFunctionObjArgs(abspath_func, s, NULL);
  }
  else {
    path = s;
    Py_INCREF(s);
  }

  assert(path);
  assert(ignore_paths_lst);

  char* path_str = PyString_AsString(path);

  Py_ssize_t i;
  for (i = 0; i < PyList_Size(ignore_paths_lst); i++) {
    PyObject* elt = PyList_GET_ITEM(ignore_paths_lst, i);
    char* path_prefix = PyString_AsString(elt);
    // we're doing a prefix match, so remember to use strncmp
    if (strncmp(path_str, path_prefix, strlen(path_prefix)) == 0) {
      Py_DECREF(path);
      return 1;
    }
  }

  Py_DECREF(path);
  return 0;
}


/* Returns a newly-allocated PyString object that represents a canonical
   name for the given code object:

     "$function_name [$filename]" if it's a regular function
     "$classname::$function_name [$filename]" if it's a method

   To canonicalize file paths, we use os.path.abspath to grab the
   ABSOLUTE PATH of the filename.  If there is some error in creating
   a canonical name, then return NULL. */
static PyObject* create_canonical_code_name(PyCodeObject* this_code) {
  PyObject* classname = this_code->co_classname; // could be NULL

  PyObject* name = this_code->co_name;
  PyObject* filename = this_code->co_filename;

  assert(PyString_CheckExact(name));
  assert(PyString_CheckExact(filename));

  PyObject* filename_abspath = PyObject_CallFunctionObjArgs(abspath_func, filename, NULL);

  // this fails *mysteriously* in numpy when doing:
  //
  //  import numpy
  //  numpy.random.mtrand.shuffle([1])
  if (!filename_abspath) {
    assert(PyErr_Occurred());
    // trying to clear the error results in a segfault, this is WEIRD!
    return NULL;
  }

  assert(filename_abspath);

  PyObject* ret = NULL;

  if (classname) {
    assert(PyString_CheckExact(classname));

    PyObject* canonical_method_str = PyString_FromString("%s::%s [%s]");
    PyObject* format_triple = PyTuple_Pack(3, classname, name, filename_abspath);
    ret = PyString_Format(canonical_method_str, format_triple);
    Py_DECREF(format_triple);
    Py_DECREF(canonical_method_str);
  }
  else {
    PyObject* canonical_func_str = PyString_FromString("%s [%s]");
    PyObject* format_pair = PyTuple_Pack(2, name, filename_abspath);
    ret = PyString_Format(canonical_func_str, format_pair);
    Py_DECREF(format_pair);
    Py_DECREF(canonical_func_str);
  }

  assert(ret);
  Py_DECREF(filename_abspath);

  return ret;
}

// simultaneously initialize the pg_ignore and pg_canonical_name
// fields, and then call add_new_code_dep
//
// We do this at the time when a code object is created, so that we
// don't have to do these checks every time a function is called.
void pg_init_new_code_object(PyCodeObject* co) {
  // set defaults ...
  co->pg_ignore = 1; // ignore unless we have a good reason NOT to
  co->pg_canonical_name = NULL;


  // ... and only run this if we've been properly initialized
  MEMOIZE_PUBLIC_START()

  co->pg_canonical_name = create_canonical_code_name(co);


  // ignore the following code:
  //
  //   0. ignore code that we can't even create a canonical name for
  //      (presumably because create_canonical_code_name returned an error)
  //   1. ignore generators
  //   2. ignore lambda functions
  //   3. ignore code with untrackable filenames
  //   4. ignore code from files whose paths start with some element of
  //      ignore_paths_lst
  co->pg_ignore =
    ((!co->pg_canonical_name) ||

     (co->co_flags & CO_GENERATOR) ||

     (strcmp(PyString_AsString(co->co_name), "<lambda>") == 0) ||

     ((strcmp(PyString_AsString(co->co_filename), "<string>") == 0) ||
      (strcmp(PyString_AsString(co->co_filename), "<stdin>") == 0) ||
      (strcmp(PyString_AsString(co->co_filename), "???") == 0)) ||

     (prefix_in_ignore_paths_lst(co->co_filename)));


  // pg_CREATE_FUNCTION_event should catch most code dependencies, but
  // in order to catch nested functions (which aren't initialized by
  // pg_CREATE_FUNCTION_event), we need to also make a call here ...
  //
  // (see IncPy-regression-tests/nested_function_1/ for details)
  if (!co->pg_ignore) {
    add_new_code_dep(co);
  }

  MEMOIZE_PUBLIC_END()
}

// adds cod to func_name_to_code_dependency and func_name_to_code_object
void add_new_code_dep(PyCodeObject* cod) {
  if (!cod->pg_ignore) {
    PyDict_SetItem(func_name_to_code_object, cod->pg_canonical_name, (PyObject*)cod);

    PyObject* new_code_dependency = CREATE_NEW_code_dependency(cod);
    PyDict_SetItem(func_name_to_code_dependency, cod->pg_canonical_name, new_code_dependency);
    Py_DECREF(new_code_dependency);

    /* This is overkill, but what we're gonna do is clear the
       all_code_deps_SAT fields of ALL currently-existing FuncMemoInfo
       entries, since their code dependencies might have changed because
       new code might have been loaded for the SAME function name.  This
       can happen when files are loaded with execfile(), which shells
       like IPython do.  That is, in the middle of a Python interpreter
       session, the code for a function might actually change, since
       it's been reloaded from the source file.

       (See IncPy-regression-tests/execfile_2 for a test case)

       The proper solution is to keep links from each FuncMemoInfo entry
       to all of its callers, so that we can simply invalidate that
       entry and all its (transitive) callers rather than invalidating
       ALL FuncMemoInfo entries.  However, that's too much work for now :) 
    
       If this becomes a performance bottleneck, then OPTIMIZE IT! */
    PyObject* canonical_name = NULL;
    PyObject* fmi_addr = NULL;
    Py_ssize_t pos = 0;
    while (PyDict_Next(all_func_memo_info_dict, &pos, &canonical_name, &fmi_addr)) {
      FuncMemoInfo* func_memo_info = (FuncMemoInfo*)PyLong_AsLong(fmi_addr);
      func_memo_info->all_code_deps_SAT = 0;
    }
  }
}

// called when a new function object is created
void pg_CREATE_FUNCTION_event(PyFunctionObject* func) {
  MEMOIZE_PUBLIC_START()
  PyCodeObject* cod = (PyCodeObject*)(func->func_code);
  add_new_code_dep(cod);
  MEMOIZE_PUBLIC_END()
}

// called when a new class object is created
void pg_BUILD_CLASS_event(PyObject* name, PyObject* methods_dict) {
  MEMOIZE_PUBLIC_START()

  assert(PyString_CheckExact(name));

  // iterate through all objects in methods_dict, and for all functions,
  // grab their enclosed code objects and SET their co_classname field

  PyObject* attrname = NULL;
  PyObject* val = NULL;
  Py_ssize_t pos = 0;
  while (PyDict_Next(methods_dict, &pos, &attrname, &val)) {
    // we only care about values that are FUNCTIONS;
    // some members aren't functions
    if (PyFunction_Check(val)) {
      PyFunctionObject* f = (PyFunctionObject*)val;
      PyCodeObject* cod = (PyCodeObject*)f->func_code;

      // don't bother doing this for ignored code
      if (cod->pg_ignore) {
        continue;
      }

      assert(cod->pg_canonical_name);

      // THIS IS REALLY SUBTLE BUT IMPORTANT ... by this point, there
      // should already be an entry in func_name_to_code_dependency with
      // the original canonical name of cod (before its co_classname is
      // set) ... we need to DELETE that entry, so that we don't have
      // TWO entries for the same code object (one without co_classname
      // and the other one with co_classname):
      PyObject* old_canonical_name = cod->pg_canonical_name;

      // subtle! sometimes the entry for old_canonical_name might have
      // already been deleted, if, say there were an identically-named
      // method shared by 2 or more classes NESTED within one another
      if (PyDict_Contains(func_name_to_code_dependency, old_canonical_name)) {
        PyDict_DelItem(func_name_to_code_dependency, old_canonical_name);
      }

      // keep func_name_to_code_object in-sync with
      // func_name_to_code_dependency ...
      if (PyDict_Contains(func_name_to_code_object, old_canonical_name)) {
        PyDict_DelItem(func_name_to_code_object, old_canonical_name);
      }


      // i'm pretty sure this should only be set ONCE, unless someone's
      // doing uber-advanced Python trickery ...
      // but issue a warning nonetheless
      if (cod->co_classname) {
        PG_LOG_PRINTF("dict(event='WARNING', what='class name set more than once', why='old=%s, new=%s')\n",
                      PyString_AsString(cod->co_classname),
                      PyString_AsString(name));
        Py_CLEAR(cod->co_classname); // blow away old version
      }

      // set its new name:
      cod->co_classname = name;
      Py_INCREF(name); // very important!

      // also update pg_canonical_name to reflect new co_classname
      Py_CLEAR(cod->pg_canonical_name); // nullify old value

      cod->pg_canonical_name = create_canonical_code_name(cod);

      // create a fresh new code_dependency object for the method
      // with its newly-set co_classname field
      add_new_code_dep(cod);
    }
  }

  MEMOIZE_PUBLIC_END()
}


static void mark_impure(PyFrameObject* f, char* why) {
  if (!f->func_memo_info) return;

  // don't log more than once per function
  if (f->func_memo_info->is_impure) {
    return;
  }

  // only print log message for functions we're not ignoring:
  if (!f->f_code->pg_ignore) {
    PG_LOG_PRINTF("dict(event='MARK_IMPURE', what='%s', why='%s')\n",
                  PyString_AsString(f->f_code->pg_canonical_name), why);
  }

  f->func_memo_info->is_impure = 1;
}

static void mark_entire_stack_impure(char* why) {
  PyFrameObject* f = PyEval_GetFrame();
  while (f) {
    mark_impure(f, why);
    f = f->f_back;
  }
}

void pg_MARK_IMPURE_event(char* why) {
  MEMOIZE_PUBLIC_START()
  mark_entire_stack_impure(why);
  MEMOIZE_PUBLIC_END()
}


// translates the canonical name func_name into a newly-allocated string
// that can be used as part of a filename that uniquely identifies it:
PyObject* canonical_name_to_filename(PyObject* func_name) {
  PyObject* md5_func = PyObject_CallFunctionObjArgs(hashlib_md5_func, func_name, NULL);
  PyObject* tmp_str = PyString_FromString("hexdigest");

  // Perform: hashlib.md5(func_name).hexdigest()
  PyObject* hexdigest = PyObject_CallMethodObjArgs(md5_func, tmp_str, NULL);
  Py_DECREF(md5_func);
  Py_DECREF(tmp_str);

  return hexdigest;
}


// called at the beginning of execution
void pg_initialize() {
#ifdef DISABLE_MEMOIZE
  return;
#endif

  assert(!pg_activated);


  if ((sizeof(UInt16) != 2) ||
      (sizeof(UInt32) != 4) ||
      (sizeof(UInt64) != 8)) {
    fprintf(stderr, "ERROR: UInt16, UInt32, UInt64 types have the wrong sizes.\n");
    Py_Exit(1);
  }

// initialize and zero out level 1 mappings:
#ifdef HOST_IS_64BIT
// 64-bit architecture
  if (sizeof(void*) != 8) {
    fprintf(stderr, "ERROR: void* isn't 64 bits.\n");
    Py_Exit(1);
  }

  level_1_map = PyMem_New(obj_metadata***, METADATA_MAP_SIZE);
  memset(level_1_map, 0, sizeof(*level_1_map) * METADATA_MAP_SIZE);
#else
// 32-bit architecture
  if (sizeof(void*) != 4) {
    fprintf(stderr, "ERROR: void* isn't 32 bits.\n");
    Py_Exit(1);
  }

  level_1_map = PyMem_New(obj_metadata_array*, METADATA_MAP_SIZE);
  memset(level_1_map, 0, sizeof(obj_metadata_array*) * METADATA_MAP_SIZE);
#endif


#ifdef ENABLE_DEBUG_LOGGING // defined in "memoize_logging.h"
  // TODO: is there a more portable way to do logging?  what about on
  // Windows?  We should probably just create a PyFileObject and use its
  // various write methods to do portable file writing :)
  // (but then we can't easily do fprintf into it)
  debug_log_file = fopen("memoize.log", "w");
#endif

  // import some useful Python modules, so that we can call their functions:

  PyObject* copy_module = PyImport_ImportModule("copy"); // increments refcount
  assert(copy_module);
  deepcopy_func = PyObject_GetAttrString(copy_module, "deepcopy");
  Py_DECREF(copy_module);
  assert(deepcopy_func);

  PyObject* cPickle_module = PyImport_ImportModule("cPickle"); // increments refcount

  // this is the first C extension module we're trying to import, so do
  // a check to make sure it exists, and if not, simply punt on
  // initialization altogether.  This will ensure that IncPy can compile
  // after a 'make clean' (since cPickle doesn't exist yet at that time)
  if (!cPickle_module) {
    fprintf(stderr, "WARNING: cPickle module doesn't yet exist, so IncPy features not activated.\n");
    return;
  }

  assert(cPickle_module);
  cPickle_dump_func = PyObject_GetAttrString(cPickle_module, "dump");
  cPickle_dumpstr_func = PyObject_GetAttrString(cPickle_module, "dumps");
  cPickle_load_func = PyObject_GetAttrString(cPickle_module, "load");
  Py_DECREF(cPickle_module);

  assert(cPickle_dump_func);
  assert(cPickle_dumpstr_func);
  assert(cPickle_load_func);

  PyObject* hashlib_module = PyImport_ImportModule("hashlib"); // increments refcount
  hashlib_md5_func = PyObject_GetAttrString(hashlib_module, "md5");
  Py_DECREF(hashlib_module);
  assert(hashlib_md5_func);

  PyObject* os_module = PyImport_ImportModule("os"); // increments refcount
  PyObject* path_module = PyObject_GetAttrString(os_module, "path");
  abspath_func = PyObject_GetAttrString(path_module, "abspath");
  assert(abspath_func);

  ignore_str = PyString_FromString("IGNORE");


  // look for the mandatory incpy.config file in $HOME
  // using os.path.join(os.getenv('HOME'), 'incpy.config')
  PyObject* getenv_func = PyObject_GetAttrString(os_module, "getenv");
  PyObject* join_func = PyObject_GetAttrString(path_module, "join");

  PyObject* tmp_str = PyString_FromString("HOME");
  PyObject* homedir = PyObject_CallFunctionObjArgs(getenv_func, tmp_str, NULL);
  assert(homedir);
  Py_DECREF(tmp_str);

  tmp_str = PyString_FromString("incpy.config");
  PyObject* incpy_config_path = PyObject_CallFunctionObjArgs(join_func, homedir, tmp_str, NULL);
  assert(incpy_config_path);
  Py_DECREF(tmp_str);

  tmp_str = PyString_FromString("incpy.aggregate.log");
  PyObject* incpy_log_path = PyObject_CallFunctionObjArgs(join_func, homedir, tmp_str, NULL);
  assert(incpy_log_path);

  // Also open $HOME/incpy.aggregate.log (in append mode) while you're at it:
  user_aggregate_log_file = fopen(PyString_AsString(incpy_log_path), "a");

  user_log_file = fopen("incpy.log", "w");

  Py_DECREF(incpy_log_path);
  Py_DECREF(tmp_str);
  Py_DECREF(homedir);

  // parse incpy.config to look for lines of the following form:
  //   ignore = <prefix of path to ignore>
  //   time_limit = <time limit in SECONDS>

  ignore_paths_lst = PyList_New(0);

  PyObject* config_file = PyFile_FromString(PyString_AsString(incpy_config_path), "r");
  if (!config_file) {
    fprintf(stderr, "ERROR: IncPy config file not found.  Create an empty file with this command:\n\ntouch %s\n\nFor better performance, ignore all standard library code by\nadding this line to your incpy.config file:\n\nignore = <absolute path to IncPy installation directory>\n",
            PyString_AsString(incpy_config_path));
    assert(PyErr_Occurred());
    PyErr_Clear();
    Py_Exit(1);
  }

  PyObject* readlines_func = PyObject_GetAttrString(config_file, "readlines");
  PyObject* all_lines = PyObject_CallFunctionObjArgs(readlines_func, NULL);
  PyObject* split_str = PyString_FromString("split");
  PyObject* strip_str = PyString_FromString("strip");
  PyObject* equal_str = PyString_FromString("=");

  Py_ssize_t i;
  for (i = 0; i < PyList_Size(all_lines); i++) {
    PyObject* line = PyList_GET_ITEM(all_lines, i);
    PyObject* toks = PyObject_CallMethodObjArgs(line, split_str, equal_str, NULL);
    assert(toks);
    if (PyList_Size(toks) == 2) {
      PyObject* lhs = PyList_GET_ITEM(toks, 0);
      PyObject* rhs = PyList_GET_ITEM(toks, 1);
      // strip both:
      PyObject* lhs_stripped = PyObject_CallMethodObjArgs(lhs, strip_str, NULL);
      PyObject* rhs_stripped = PyObject_CallMethodObjArgs(rhs, strip_str, NULL);

      // 'ignore = <path prefix>'
      if (strcmp(PyString_AsString(lhs_stripped), "ignore") == 0) {
        // first grab the absolute path:
        PyObject* ignore_abspath = PyObject_CallFunctionObjArgs(abspath_func, rhs_stripped, NULL);

        // then check to see whether it's an existent file or directory
        PyObject* exists_func = PyObject_GetAttrString(path_module, "exists");
        PyObject* path_exists_bool = PyObject_CallFunctionObjArgs(exists_func, ignore_abspath, NULL);
        Py_DECREF(exists_func);

        if (!PyObject_IsTrue(path_exists_bool)) {
          fprintf(stderr, "ERROR: The ignore path %s\n       specified in incpy.config does not exist\n",
                  PyString_AsString(ignore_abspath));
          Py_Exit(1);
        }

        // SUBTLE!  if it's a directory, then add a trailing '/' so that
        // when we do a prefix match later with filenames, we don't get 
        // spurious matches with directory names that have this
        // directory as a prefix
        PyObject* isdir_func = PyObject_GetAttrString(path_module, "isdir");
        PyObject* path_isdir_bool = PyObject_CallFunctionObjArgs(isdir_func, ignore_abspath, NULL);
        Py_DECREF(isdir_func);

        if (PyObject_IsTrue(path_isdir_bool)) {
          PyObject* slash = PyString_FromString("/");
          PyString_Concat(&ignore_abspath, slash);
          Py_DECREF(slash);
        }

        PyList_Append(ignore_paths_lst, ignore_abspath);

        Py_DECREF(path_isdir_bool);
        Py_DECREF(path_exists_bool);
        Py_DECREF(ignore_abspath);
      }
      // 'time_limit = <time limit in SECONDS>'
      else if (strcmp(PyString_AsString(lhs_stripped), "time_limit") == 0) {
        // try to see if it's a valid Python long object
        PyObject* time_limit_sec_obj =
          PyLong_FromString(PyString_AsString(rhs_stripped), NULL, 0);

        if (time_limit_sec_obj) {
          long time_limit_sec = PyLong_AsLong(time_limit_sec_obj);
          if (time_limit_sec > 0) {
            // multiply by 1000 to convert to ms ...
            memoize_time_limit_ms = (unsigned int)time_limit_sec * 1000;
          }
          else {
            fprintf(stderr, "ERROR: Invalid time_limit %ld in incpy.config\n       (must specify a non-negative integer)\n", time_limit_sec);
            Py_Exit(1);
          }
          Py_DECREF(time_limit_sec_obj);
        }
        else {
          assert(PyErr_Occurred());
          PyErr_Clear();
          fprintf(stderr, "ERROR: Invalid time_limit '%s' in incpy.config\n       (must specify a non-negative integer)\n", 
                  PyString_AsString(rhs_stripped));
          Py_Exit(1);
        }
      }

      Py_DECREF(lhs_stripped);
      Py_DECREF(rhs_stripped);
    }

    Py_DECREF(toks);
  }

  Py_DECREF(equal_str);
  Py_DECREF(strip_str);
  Py_DECREF(split_str);
  Py_DECREF(all_lines);
  Py_DECREF(readlines_func);
  Py_DECREF(config_file);
  Py_DECREF(incpy_config_path);

  Py_DECREF(getenv_func);
  Py_DECREF(join_func);
  Py_DECREF(path_module);
  Py_DECREF(os_module);


  // global data structures:
  global_containment_intern_cache = PyDict_New();
  func_name_to_code_dependency = PyDict_New();
  func_name_to_code_object = PyDict_New();
  all_func_memo_info_dict = PyDict_New();
  cow_containment_dict = PyDict_New();
  cow_traced_addresses_set = PySet_New(NULL);


  char time_buf[100];
  time_t t = time(NULL);
  struct tm* tmp_tm = localtime(&t);
  strftime(time_buf, 100, "%Y-%m-%d %T", tmp_tm);


  assert(ignore_paths_lst);
  tmp_str = PyObject_Repr(ignore_paths_lst);
  USER_LOG_PRINTF("=== %s START | TIME_LIMIT %u sec | IGNORE %s",
                  time_buf,
                  memoize_time_limit_ms / 1000,
                  PyString_AsString(tmp_str));
  Py_DECREF(tmp_str);
  if (trust_prev_memoized_results) {
    USER_LOG_PRINTF(" | TRUST_PREV_RESULTS\n");
  }
  else {
    USER_LOG_PRINTF("\n");
  }

  init_self_mutator_c_methods();
  init_definitely_impure_funcs();

  PycString_IMPORT;

  pg_activated = 1;
}

// called at the end of execution
void pg_finalize() {
#ifdef DISABLE_MEMOIZE
  return;
#endif

  if (!pg_activated) {
    return;
  }

  // turn this off ASAP so that we don't have to worry about disabling
  // it later temporarily when we're calling cPickle_dump_func, etc.
  pg_activated = 0;

  TrieFree(self_mutator_c_methods);
  TrieFree(definitely_impure_funcs);

  struct stat st;
  // create incpy-cache/ sub-directory if it doesn't already exist
  if (stat("incpy-cache", &st) != 0) {
    mkdir("incpy-cache", 0777);
  }

  PyObject* negative_one = PyInt_FromLong(-1);

  // serialize and pickle func_memo_info entries to disk
  PyObject* canonical_name = NULL;
  PyObject* fmi_addr = NULL;
  Py_ssize_t pos = 0;
  while (PyDict_Next(all_func_memo_info_dict, &pos, &canonical_name, &fmi_addr)) {
    FuncMemoInfo* func_memo_info = (FuncMemoInfo*)PyLong_AsLong(fmi_addr);

    PyObject* func_name = GET_CANONICAL_NAME(func_memo_info);
    PyObject* basename = canonical_name_to_filename(func_name);

    PyObject* deps_fn =
      PyString_FromFormat("incpy-cache/%s.dependencies.pickle",
                          PyString_AsString(basename));

    // pickle dependencies to disk:
    PyObject* serialized_deps =
      serialize_func_memo_info_dependencies(func_memo_info);

    PyObject* deps_outfile = PyFile_FromString(PyString_AsString(deps_fn), "wb");
    assert(deps_outfile);
    // pass in -1 to force cPickle to use a binary protocol
    PyObject* cPickle_dump_res =
      PyObject_CallFunctionObjArgs(cPickle_dump_func,
                                   serialized_deps,
                                   deps_outfile,
                                   negative_one, NULL);

    // note that pickling might still fail if there's something inside
    // of serialized_deps that's not picklable (sadly, our
    // is_picklable() implementation doesn't pick up everything)
    if (cPickle_dump_res) {
      Py_DECREF(cPickle_dump_res);
    }
    else {
      assert(PyErr_Occurred());
      PyErr_Clear();

      PG_LOG_PRINTF("dict(event='WARNING', what='dependencies cannot be pickled', funcname='%s')\n",
                    PyString_AsString(func_name));
    }

    Py_DECREF(deps_outfile);
    Py_DECREF(serialized_deps);
    Py_DECREF(deps_fn);

    // separately pickle memoized vals ONLY IF it's been loaded
    // from disk (or otherwise updated by a clear_cache_and_mark_pure)
    if (func_memo_info->memoized_vals_loaded) {
      PyObject* memoized_vals_fn =
        PyString_FromFormat("incpy-cache/%s.memoized_vals.pickle",
                            PyString_AsString(basename));

      // if it exists, then pickle it
      if (func_memo_info->memoized_vals) {
        PyObject* memoized_vals_outfile =
          PyFile_FromString(PyString_AsString(memoized_vals_fn), "wb");
        assert(memoized_vals_outfile);

        // pass in -1 to force cPickle to use a binary protocol
        PyObject* cPickle_dump_res =
          PyObject_CallFunctionObjArgs(cPickle_dump_func,
                                       func_memo_info->memoized_vals,
                                       memoized_vals_outfile,
                                       negative_one, NULL);
        if (cPickle_dump_res) {
          Py_DECREF(cPickle_dump_res);
        }
        else {
          assert(PyErr_Occurred());
          PyErr_Clear();

          PG_LOG_PRINTF("dict(event='WARNING', what='memoized vals cannot be pickled', funcname='%s')\n",
                        PyString_AsString(func_name));
        }

        Py_DECREF(memoized_vals_outfile);
      }
      // REALLY important!!!  If memoized_vals is non-existent, then
      // we should DELETE the corresponding pickle file since the LACK
      // of a file corresponds to an empty memoized_vals ...
      else {
        unlink(PyString_AsString(memoized_vals_fn));
      }

      Py_DECREF(memoized_vals_fn);
    }

    Py_DECREF(basename);
  }

  Py_DECREF(negative_one);


  // deallocate all FuncMemoInfo entries:
  canonical_name = NULL;
  fmi_addr = NULL;
  pos = 0; // reset it!
  while (PyDict_Next(all_func_memo_info_dict, &pos, &canonical_name, &fmi_addr)) {
    FuncMemoInfo* func_memo_info = (FuncMemoInfo*)PyLong_AsLong(fmi_addr);
    DELETE_func_memo_info(func_memo_info);
  }
  Py_CLEAR(all_func_memo_info_dict);

  Py_CLEAR(global_containment_intern_cache);
  Py_CLEAR(func_name_to_code_dependency);
  Py_CLEAR(func_name_to_code_object);
  Py_CLEAR(cow_containment_dict);
  Py_CLEAR(cow_traced_addresses_set);
  Py_CLEAR(ignore_paths_lst);

  // function pointers
  Py_CLEAR(deepcopy_func);
  Py_CLEAR(cPickle_dumpstr_func);
  Py_CLEAR(cPickle_dump_func);
  Py_CLEAR(cPickle_load_func);
  Py_CLEAR(hashlib_md5_func);
  Py_CLEAR(abspath_func);
  Py_CLEAR(numpy_module);

  Py_CLEAR(ignore_str);


#ifdef ENABLE_DEBUG_LOGGING // defined in "memoize_logging.h"
  fclose(debug_log_file);
  debug_log_file = NULL;
#endif

  char time_buf[100];
  time_t t = time(NULL);
  struct tm* tmp_tm = localtime(&t);
  strftime(time_buf, 100, "%Y-%m-%d %T", tmp_tm);

  USER_LOG_PRINTF("=== %s END\n\n", time_buf);

  fclose(user_aggregate_log_file);
  user_aggregate_log_file = NULL;

  fclose(user_log_file);
  user_log_file = NULL;
}


/* checks code dependencies for the current function
   returns 1 if dependencies satisfied, 0 if they're not */
static int are_code_dependencies_satisfied(FuncMemoInfo* my_func_memo_info,
                                           PyFrameObject* cur_frame) {
  // you must at least have a code dependency on YOURSELF
  assert(my_func_memo_info->code_dependencies);

  // Optimization: assuming that code doesn't change in the middle of
  // execution (which is a pretty safe assumption), we only need to do
  // this check ONCE at the beginning of execution and not on each call
  //
  // Caveat: if code is re-loaded in the middle of execution, it CAN
  // actually change, so all_code_deps_SAT must be set to 0 in those
  // cases (grep for 'all_code_deps_SAT' to see where that occurs)
  if (!my_func_memo_info->all_code_deps_SAT) {
    PyObject* dependent_func_canonical_name = NULL;
    PyObject* memoized_code_dependency = NULL;
    Py_ssize_t pos = 0;
    while (PyDict_Next(my_func_memo_info->code_dependencies,
                       &pos, &dependent_func_canonical_name, &memoized_code_dependency)) {
      PyObject* cur_code_dependency =
        PyDict_GetItem(func_name_to_code_dependency, dependent_func_canonical_name);

      char* dependent_func_name_str = PyString_AsString(dependent_func_canonical_name);
      // if the function is NOT FOUND or its code has changed, then
      // that code dependency is definitely broken
      if (!cur_code_dependency) {
        PG_LOG_PRINTF("dict(event='CODE_DEPENDENCY_BROKEN', why='CODE_NOT_FOUND', what='%s')\n",
                      dependent_func_name_str);
        USER_LOG_PRINTF("CODE_DEPENDENCY_BROKEN %s | %s not found\n",
                        PyString_AsString(GET_CANONICAL_NAME(my_func_memo_info)),
                        dependent_func_name_str);
        return 0;
      }
      else if (!code_dependency_EQ(cur_code_dependency, memoized_code_dependency)) {
        PG_LOG_PRINTF("dict(event='CODE_DEPENDENCY_BROKEN', why='CODE_CHANGED', what='%s')\n",
                      dependent_func_name_str);
        USER_LOG_PRINTF("CODE_DEPENDENCY_BROKEN %s | %s changed\n",
                        PyString_AsString(GET_CANONICAL_NAME(my_func_memo_info)),
                        dependent_func_name_str);
        return 0;
      }
    }

    // if we make it this far without hitting an early 'return 0',
    // then this function's code dependencies are satisfied for this
    // particular script execution, so we don't have to check it again:
    my_func_memo_info->all_code_deps_SAT = 1;
  }

  return 1;
}


// returns a PyObject* if we can re-use a memoized result and NULL if we cannot
PyObject* pg_enter_frame(PyFrameObject* f) {
  MEMOIZE_PUBLIC_START_RETNULL()

  // mark entire stack impure when calling Python functions with names
  // matching definitely_impure_funcs
  //
  // TODO: could speed up if necessary by adding an extra field in
  // the code object (Include/code.h) and doing checking against
  // definitely_impure_funcs at code creation time rather than call time
  if (TrieContains(definitely_impure_funcs, PyString_AsString(f->f_code->co_name))) {
    mark_entire_stack_impure(PyString_AsString(f->f_code->co_name));
    MEMOIZE_PUBLIC_END() // remember these error paths!
    return NULL;
  }

  // if this function's code should be ignored, then return early:
  if (f->f_code->pg_ignore) {
    MEMOIZE_PUBLIC_END() // remember these error paths!
    return NULL;
  }

  BEGIN_TIMING(f->start_time);

  // TODO: is this the right place to put this?  I gotta put it before
  // setting f->start_func_call_time, but should I also count ignored
  // functions or not?
  num_executed_func_calls++;

  f->start_func_call_time = num_executed_func_calls;

  PyCodeObject* co = f->f_code;

  // punt EARLY for all top-level modules without creating a
  // func_memo_info field for them:
  if (co->pg_is_module) {
    goto pg_enter_frame_done;
  }

  f->func_memo_info = get_func_memo_info_from_cod(co);

  // update ALL callers with a code dependency on you
  // (TODO: can optimize later by only updating your immediate caller and
  //  then 'bubbling up' dependencies when it exits)
  PyFrameObject* cur_frame = f->f_back;
  PyObject* my_self_code_dependency = NULL;
  while (cur_frame) {
    if (cur_frame->func_memo_info) {
      PyObject* caller_code_deps = cur_frame->func_memo_info->code_dependencies;
      assert(caller_code_deps);

      // Optimizaton: avoid adding duplicates (there will be MANY duplicates
      // throughout a long-running script with lots of function calls)
      if (!PyDict_Contains(caller_code_deps, co->pg_canonical_name)) {
        // add a code dependency in your caller to the most up-to-date
        // code for THIS function (not a previously-saved version of the
        // code, since later we might discover it to be outdated!)

        // lazy-initialize
        if (!my_self_code_dependency) {
          my_self_code_dependency =
            PyDict_GetItem(func_name_to_code_dependency, co->pg_canonical_name);
          assert(my_self_code_dependency);
        }

        PyDict_SetItem(caller_code_deps, co->pg_canonical_name, my_self_code_dependency);
      }
    }
    cur_frame = cur_frame->f_back;
  }


  // punt on all impure functions ... we can't memoize their return values
  if (f->func_memo_info->is_impure) {
    goto pg_enter_frame_done;
  }


  if (!are_code_dependencies_satisfied(f->func_memo_info, f)) {
    if (trust_prev_memoized_results) {
      fprintf(stderr, "WARNING: trusting possibly outdated results for %s\n",
              PyString_AsString(co->pg_canonical_name));
      USER_LOG_PRINTF("TRUSTING_MEMOIZED_RESULTS %s\n", PyString_AsString(co->pg_canonical_name));
    }
    else {
      clear_cache_and_mark_pure(f->func_memo_info);
      USER_LOG_PRINTF("CLEAR_CACHE %s\n", PyString_AsString(co->pg_canonical_name));
      goto pg_enter_frame_done;
    }
  }


  // by now, all code dependencies are still satisfied ...

  PyObject* memoized_vals_lst = get_memoized_vals_lst(f->func_memo_info);

  /* TODO: this is REALLY EXPENSIVE if memoized_vals_lst and/or
     its constituent elements are LARGE, since we're just doing a 
     linear search down memoized_vals_lst, doing an obj_equals 
     rich-comparison on each element.
   */
  PyObject* memoized_retval = NULL;
  PyObject* memoized_stdout_buf = NULL;
  PyObject* memoized_stderr_buf = NULL;

  long memoized_runtime_ms = -1;

  if (memoized_vals_lst) {
    Py_ssize_t memoized_vals_idx;
    for (memoized_vals_idx = 0;
         memoized_vals_idx < PyList_Size(memoized_vals_lst);
         memoized_vals_idx++) {
      PyObject* elt = PyList_GET_ITEM(memoized_vals_lst, memoized_vals_idx);
      assert(PyDict_CheckExact(elt));

      // try to find whether the function has been called before with
      // this combination of arguments and global variable values:
      int all_args_and_global_vals_equal = 1;

      PyObject* memoized_arg_lst = PyDict_GetItemString(elt, "args");
      assert(memoized_arg_lst);

      // let's iterate through the lists element-by-element
      // so that we can apply the hash consing COW optimization if
      // applicable:
      Py_ssize_t memoized_arg_lst_len = PyList_Size(memoized_arg_lst);

      if (memoized_arg_lst_len != f->f_code->co_argcount) {
        continue;
      }

      Py_ssize_t j = -1;
      for (j = 0; j < memoized_arg_lst_len; j++) {
        PyObject* memoized_elt = PyList_GET_ITEM(memoized_arg_lst, j);
        PyObject* actual_elt   = f->f_localsplus[j];

        // compare proxy objects rather than real objects if necessary,
        // since memoized_elt might be a proxy object
        PyObject* proxy_elt = create_proxy_object(actual_elt);
        if (proxy_elt) {
          actual_elt = proxy_elt; // allocs a new object if non-null
        }
        else {
          Py_INCREF(actual_elt); // to match what happens in the other branch
        }

        // obj_equals is potentially expensive to compute ...
        if (!obj_equals(memoized_elt, actual_elt)) {
          all_args_and_global_vals_equal = 0;
          Py_DECREF(actual_elt); // tricky tricky!
          break;
        }
        else {
          /* Optimization: If memoized_elt and actual_elt point to different
             objects whose values are EQUAL, then simply replace
             &memoized_elt (the appropriate element within memoized_arg_lst)
             with actual_elt (the ACTUAL parameter value) so that future
             comparisons are lightning fast (since their pointers are now
             equal).  
          
             The one caveat is that this must be a COW copy, since it's
             possible for someone to mutate actual_elt and have its value
             actually NOT be equal to memoized_elt any longer.  In that
             case, we should set &memoized_elt (the appropriate element
             within memoized_arg_lst) back to its original value.

             UPDATE: Let's disable the hash consing optimization for
             now, since the call to cow_containment_dict_ADD REALLY
             slows things down in some benchmarks by shooting the memory
             usage THROUGH THE ROOF! */
#ifdef USE_HASH_CONSING_OPT

#ifdef ENABLE_COW
          // Remember, do this only their values are equal but their
          // addresses are NOT EQUAL ...
          if (memoized_elt != actual_elt) {
            PyList_SetItem(memoized_arg_lst, j, actual_elt);
            Py_INCREF(actual_elt); // PyList_SetItem doesn't incref the new item
            // make it a COW copy ...
            cow_containment_dict_ADD(actual_elt);
          }
#endif // ENABLE_COW

#endif // USE_HASH_CONSING_OPT
        }

        Py_DECREF(actual_elt); // tricky tricky!
      }

      if (!all_args_and_global_vals_equal) {
        continue; // ... onto next element of memoized_vals_lst
      }

      // we've matched up the args, now see if global vars match ...
      PyObject* global_vars_read = PyDict_GetItemString(elt, "global_vars_read");
      if (global_vars_read) {
        PyObject* global_varname_tuple = NULL;
        PyObject* memoized_value = NULL;
        Py_ssize_t pos = 0;
        while (PyDict_Next(global_vars_read,
                           &pos, &global_varname_tuple, &memoized_value)) {
          PyObject* cur_value =
            find_globally_reachable_obj_by_name(global_varname_tuple, f);

          if (!cur_value) {
            // we can't even find the global object, then PUNT!
#ifdef ENABLE_DEBUG_LOGGING
            PyObject* tmp_str = PyObject_Repr(global_varname_tuple);
            char* varname_str = PyString_AsString(tmp_str);
            PG_LOG_PRINTF("dict(warning='GLOBAL VAR NOT FOUND, varname=\"%s\")\n",
                          varname_str);
            Py_DECREF(tmp_str);
#endif // ENABLE_DEBUG_LOGGING

            all_args_and_global_vals_equal = 0;
            break;
          }
          else if (!obj_equals(memoized_value, cur_value)) {
            all_args_and_global_vals_equal = 0;
            break;
          }
        }
      }

      if (!all_args_and_global_vals_equal) {
        continue; // ... onto next element of memoized_vals_lst
      }


      // now that we've found a match, check to make sure files_read and
      // files_written haven't yet been modified.  if any of these files
      // have been modified, then we must CLEAR ONLY THIS ENTRY in
      // memoized_vals and break out of the loop
      char dependencies_satisfied = 1;

      PyObject* files_read = PyDict_GetItemString(elt, "files_read");
      if (files_read) {
        PyObject* dependent_filename = NULL;
        PyObject* saved_modtime_obj = NULL;
        Py_ssize_t pos = 0;
        while (PyDict_Next(files_read,
                           &pos, &dependent_filename, &saved_modtime_obj)) {
          char* dependent_filename_str = PyString_AsString(dependent_filename);
          PyFileObject* fobj = (PyFileObject*)PyFile_FromString(dependent_filename_str, "r");

          if (fobj) {
            long mtime = (long)PyOS_GetLastModificationTime(PyString_AsString(fobj->f_name),
                                                            fobj->f_fp);
            Py_DECREF(fobj);
            long saved_modtime = PyInt_AsLong(saved_modtime_obj);
            if (mtime != saved_modtime) {
              PG_LOG_PRINTF("dict(event='FILE_READ_DEPENDENCY_BROKEN', why='FILE_CHANGED', what='%s')\n",
                            dependent_filename_str);
              USER_LOG_PRINTF("FILE_READ_DEPENDENCY_BROKEN %s | %s changed\n",
                              PyString_AsString(co->pg_canonical_name),
                              dependent_filename_str);
              dependencies_satisfied = 0;
              break;
            }
          }
          else {
            assert(PyErr_Occurred());
            PyErr_Clear();
            PG_LOG_PRINTF("dict(event='FILE_READ_DEPENDENCY_BROKEN', why='FILE_NOT_FOUND', what='%s')\n",
                          dependent_filename_str);
            USER_LOG_PRINTF("FILE_READ_DEPENDENCY_BROKEN %s | %s not found\n",
                            PyString_AsString(co->pg_canonical_name),
                            dependent_filename_str);
            dependencies_satisfied = 0;
            break;
          }
        }
      }

      PyObject* files_written = PyDict_GetItemString(elt, "files_written");
      if (files_written) {
        PyObject* dependent_filename = NULL;
        PyObject* saved_modtime_obj = NULL;
        Py_ssize_t pos = 0;
        while (PyDict_Next(files_written,
                           &pos, &dependent_filename, &saved_modtime_obj)) {
          char* dependent_filename_str = PyString_AsString(dependent_filename);

          PyFileObject* fobj = (PyFileObject*)PyFile_FromString(dependent_filename_str, "r");

          if (fobj) {
            long mtime = (long)PyOS_GetLastModificationTime(PyString_AsString(fobj->f_name),
                                                            fobj->f_fp);
            Py_DECREF(fobj);
            long saved_modtime = PyInt_AsLong(saved_modtime_obj);
            if (mtime != saved_modtime) {
              PG_LOG_PRINTF("dict(event='FILE_WRITE_DEPENDENCY_BROKEN', why='FILE_CHANGED', what='%s')\n",
                            dependent_filename_str);
              USER_LOG_PRINTF("FILE_WRITE_DEPENDENCY_BROKEN %s | %s changed\n",
                              PyString_AsString(co->pg_canonical_name),
                              dependent_filename_str);
              dependencies_satisfied = 0;
              break;
            }
          }
          else {
            assert(PyErr_Occurred());
            PyErr_Clear();
            PG_LOG_PRINTF("dict(event='FILE_WRITE_DEPENDENCY_BROKEN', why='FILE_NOT_FOUND', what='%s')\n",
                          dependent_filename_str);
            USER_LOG_PRINTF("FILE_WRITE_DEPENDENCY_BROKEN %s | %s not found\n",
                            PyString_AsString(co->pg_canonical_name),
                            dependent_filename_str);
            dependencies_satisfied = 0;
            break;
          }
        }
      }

      if (!dependencies_satisfied) {
        // KILL THIS ENTRY!!!
        PyObject* tmp_idx = PyLong_FromLong((long)memoized_vals_idx);
        PyObject_DelItem(memoized_vals_lst, tmp_idx);
        Py_DECREF(tmp_idx);

        PG_LOG_PRINTF("dict(event='CLEAR_CACHE_ENTRY', idx=%u, what'%s')\n",
                      (unsigned)memoized_vals_idx,
                      PyString_AsString(co->pg_canonical_name));

        break; // get the heck out of this loop!!!
      }


      // remember that retval is actually a singleton list ...
      PyObject* memoized_retval_lst = PyDict_GetItemString(elt, "retval");
      assert(memoized_retval_lst &&
             PyList_CheckExact(memoized_retval_lst) &&
             PyList_Size(memoized_retval_lst) == 1);

      memoized_retval = PyList_GET_ITEM(memoized_retval_lst, 0);

      // VERY important to increment its refcount, since
      // memoized_vals_lst (its enclosing parent) will be
      // blown away soon!!!
      Py_XINCREF(memoized_retval);

      memoized_runtime_ms = PyLong_AsLong(PyDict_GetItemString(elt, "runtime_ms"));

      // these can be null since they are optional fields in the dict
      memoized_stdout_buf = PyDict_GetItemString(elt, "stdout_buf");
      memoized_stderr_buf = PyDict_GetItemString(elt, "stderr_buf");

      break; // break out of this loop, we've found the first (should be ONLY) match!
    }
  }

  // woohoo, success!  you've avoided re-executing the function!
  if (memoized_retval) {
    assert(Py_REFCNT(memoized_retval) > 0);
    PyObject* memoized_retval_copy = NULL;

    // very subtle and important ... make a DEEP COPY of memoized_retval
    // and return the copy to the caller.  See this test for why
    // we need to return a copy:
    //
    //   IncPy-regression-tests/cow_func_retval/
#ifdef ENABLE_COW
    // defer the deepcopy until elt has been mutated
    memoized_retval_copy = memoized_retval;
    cow_containment_dict_ADD(memoized_retval);
#else
    // eagerly make the deepcopy NOW!
    // this has a refcount of 1 ... might cause a memory leak
    memoized_retval_copy = deepcopy(memoized_retval);
 
    if (!memoized_retval_copy) {
      assert(PyErr_Occurred());
      // we can't keep going, since memoized_retval_copy is NULL
      // TODO: deal with this gracefully in the future rather than failing hard ...
      PyErr_Print();
      Py_Exit(1);
    }
#endif // ENABLE_COW


    // if found, print memoized buffers to stdout/stderr
    // and also append them to the buffers of all of your callers
    // so that if they are later skipped, then they can properly replay
    // your buffers (see IncPy-regression-tests/stdout_nested_2/)
    if (memoized_stdout_buf) {
      PyObject* outf = PySys_GetObject("stdout");
      PyFile_WriteString(PyString_AsString(memoized_stdout_buf), outf);

      // start with your immediate parent:
      PyFrameObject* cur_frame = f->f_back;
      while (cur_frame) {
        if (cur_frame->func_memo_info) {
          LAZY_INIT_STRINGIO_FIELD(cur_frame->stdout_cStringIO);
          PyFile_WriteString(PyString_AsString(memoized_stdout_buf),
                             cur_frame->stdout_cStringIO);
        }
        cur_frame = cur_frame->f_back;
      }
    }

    if (memoized_stderr_buf) {
      PyObject* outf = PySys_GetObject("stderr");
      PyFile_WriteString(PyString_AsString(memoized_stderr_buf), outf);

      // start with your immediate parent:
      PyFrameObject* cur_frame = f->f_back;
      while (cur_frame) {
        if (cur_frame->func_memo_info) {
          LAZY_INIT_STRINGIO_FIELD(cur_frame->stderr_cStringIO);
          PyFile_WriteString(PyString_AsString(memoized_stderr_buf),
                             cur_frame->stderr_cStringIO);
        }
        cur_frame = cur_frame->f_back;
      }
    }


    // record time BEFORE you clean up f!
    struct timeval skip_endtime;
    END_TIMING(f->start_time, skip_endtime);
    long memo_lookup_time_ms = GET_ELAPSED_MS(f->start_time, skip_endtime);
    assert(memo_lookup_time_ms >= 0);

    PG_LOG_PRINTF("dict(event='SKIP_CALL', what='%s', memo_lookup_time_ms='%ld')\n",
                  PyString_AsString(co->pg_canonical_name),
                  memo_lookup_time_ms);
    USER_LOG_PRINTF("SKIPPED %s | lookup time %ld ms | original runtime %ld ms\n",
                    PyString_AsString(co->pg_canonical_name),
                    memo_lookup_time_ms,
                    memoized_runtime_ms);

    assert(Py_REFCNT(memoized_retval_copy) > 0);

    MEMOIZE_PUBLIC_END()
    return memoized_retval_copy; // remember to return a COPY to caller
  }


// only reach here if you actually call the function, NOT if you skip it:
pg_enter_frame_done:

  /* We can now use co_argcount to figure out how many parameters are
     passed on the top of the stack.  By this point, the top of the
     stack should be populated with passed-in parameter values.  All the
     grossness of keyword and default arguments have been resolved at
     this point, yay!

     TODO: I haven't tested support for varargs yet */
  f->stored_args_lst = PyList_New(f->f_code->co_argcount);

  Py_ssize_t i;
  for (i = 0; i < f->f_code->co_argcount; i++) {
    PyObject* elt = f->f_localsplus[i];
    PyList_SET_ITEM(f->stored_args_lst, i, elt);
    Py_INCREF(elt);
  }


  PG_LOG_PRINTF("dict(event='CALL', what='%s')\n",
                PyString_AsString(co->pg_canonical_name));

  MEMOIZE_PUBLIC_END()
  return NULL;
}


// retval is the value that the function is about to return to its caller
void pg_exit_frame(PyFrameObject* f, PyObject* retval) {
  MEMOIZE_PUBLIC_START()

  // start these at NULL to prevent weird segfaults!
  PyObject* canonical_name = NULL;
  FuncMemoInfo* my_func_memo_info = NULL;
  PyObject* stored_args_lst_copy = NULL;

  // if retval is NULL, then that means some exception occurred on the
  // stack and we're in the process of "backing out" ... don't do
  // anything with regards to memoization if this is the case ...
  if (!retval) goto pg_exit_frame_done;

  if (f->f_code->pg_ignore) {
    goto pg_exit_frame_done;
  }


  if (f->func_memo_info) {
    assert(f->start_func_call_time > 0);
  }

  // we don't need to decref this ...
  canonical_name = f->f_code->pg_canonical_name;

  END_TIMING(f->start_time, f->end_time);
  long runtime_ms = GET_ELAPSED_MS(f->start_time, f->end_time);
  assert (runtime_ms >= 0);

  // always log a return event whenever pg_activated
  PG_LOG_PRINTF("dict(event='RETURN', what='%s', runtime_ms='%ld')\n", 
                PyString_AsString(canonical_name),
                runtime_ms);

  // could be null if it's code we're trying to ignore
  my_func_memo_info = f->func_memo_info;

  // don't do anything tracing for code without a func_memo_info
  if (!my_func_memo_info) {
    goto pg_exit_frame_done;
  }


  // don't bother memoizing results of short-running functions
  if (runtime_ms <= memoize_time_limit_ms) {
    // don't forget to clean up!
    goto pg_exit_frame_done;
  }


  // punt completely on all impure functions
  if (my_func_memo_info->is_impure) {
    USER_LOG_PRINTF("CANNOT_MEMOIZE %s | impure | runtime %ld ms\n",
                    PyString_AsString(canonical_name), runtime_ms);
    // don't forget to clean up!
    goto pg_exit_frame_done;
  }


  /* Don't memoize a function invocation with non-self-contained writes.
   
     A write is self-contained if this function was on the stack when
     the file was opened in pure-write mode, written to, and then closed */
  if (f->files_written_set && PySet_Size(f->files_written_set)) {
    // ok, so this function wrote to some files ... 
    // were these writes self-contained?
    Py_ssize_t s_pos = 0;
    PyObject* written_filename;
    while (_PySet_Next(f->files_written_set, &s_pos, &written_filename)) {
      // sometimes there are weird 'fake' files with names like
      // <fdopen> and <tmpfile>, so just ignore those
      char* filename_str = PyString_AsString(written_filename);
      int filename_len = strlen(filename_str);
      if (filename_str[0] == '<' && filename_str[filename_len - 1] == '>') {
        continue;
      }

      if (!(f->files_opened_w_set && 
            PySet_Contains(f->files_opened_w_set, written_filename) &&
            f->files_closed_set &&
            PySet_Contains(f->files_closed_set, written_filename))) {
        PG_LOG("dict(event='WARNING', what='CANNOT_MEMOIZE', why='Non self-contained write')");
        USER_LOG_PRINTF("CANNOT_MEMOIZE %s | non-self-contained file write | runtime %ld ms\n",
                        PyString_AsString(canonical_name), runtime_ms);
        goto pg_exit_frame_done;
      }
    }
  }


  /* Because we make deep copies of return values when we memoize them,
     we should NOT memoize return values when they contain within them
     mutable values that are externally-accessible, since if we break 
     the aliasing relation, we risk losing correctness. */
  if (contains_externally_aliased_mutable_obj(retval, f)) {
    PG_LOG("dict(event='WARNING', what='CANNOT_MEMOIZE', why='Return value contains externally-aliased mutable object')");
    USER_LOG_PRINTF("CANNOT_MEMOIZE %s | returning externally-aliased mutable object | runtime %ld ms\n",
                    PyString_AsString(canonical_name), runtime_ms);
    goto pg_exit_frame_done;
  }


  // now make a DEEPCOPY of argument values before storing them.
  // it's safe to wait until the end of this frame's execution to do so,
  // since they are guaranteed to have the same values as they did at
  // the beginning of execution.  if they were mutated, then this
  // function should be marked impure, so it should not be being
  // memoized in the first place!
  stored_args_lst_copy = PyList_New(0);

  assert(f->stored_args_lst);
  assert(PyList_Size(f->stored_args_lst) == f->f_code->co_argcount);

  Py_ssize_t i;
  for (i = 0; i < f->f_code->co_argcount; i++) {
    PyObject* elt = PyList_GET_ITEM(f->stored_args_lst, i);
    PyObject* copy = NULL;

    // first try to see if we can create a proxy object:
    copy = create_proxy_object(elt); // allocs a new object if non-null

    // if we don't have a proxy object ...
    if (copy == NULL) {
      /* don't memoize functions whose arguments don't implement any form
         of non-identity-based comparison (e.g., using __eq__ or __cmp__
         methods), since in those cases, there is no way that we can
         possibly MATCH THEM UP with their original incarnations once we
         load the memoized values from disk (the version loaded from disk
         will be a different object than the one in memory, so '==' will
         ALWAYS FAIL if a comparison method isn't implemented)

         for speed purposes, do this BEFORE checking for is_picklable()
         since has_comparison_method() is much faster! */
      if (!has_comparison_method(elt)) {
        PG_LOG_PRINTF("dict(event='WARNING', what='CANNOT_MEMOIZE', why='Arg %u of %s has no comparison method', type='%s')\n",
                      (unsigned)i,
                      PyString_AsString(canonical_name),
                      Py_TYPE(elt)->tp_name);
        USER_LOG_PRINTF("CANNOT_MEMOIZE %s | arg %u of type '%s' has no comparison method | runtime %ld ms\n",
                        PyString_AsString(canonical_name), (unsigned)i, Py_TYPE(elt)->tp_name, runtime_ms);
        goto pg_exit_frame_done;
      }

      if (is_picklable(elt)) {
#ifdef ENABLE_COW
        // defer the deepcopy until elt has been mutated
        copy = elt;
        Py_INCREF(copy); // always incref to match what happens in deepcopy_func
        cow_containment_dict_ADD(elt);
#else
        // eagerly make the deepcopy NOW!
        copy = deepcopy(elt);

        if (!copy) {
          assert(PyErr_Occurred());
          PyErr_Clear();
        }
#endif // ENABLE_COW
      }
    }

    // If ANY argument is unpicklable, then we must be conservative and
    // completely punt on trying to memoize this function
    //
    // Note that some types can be deepcopy'ed but still NOT pickled
    // (e.g., functions, modules, etc.)
    if (copy == NULL) {
      PG_LOG_PRINTF("dict(event='WARNING', what='CANNOT_MEMOIZE', why='Arg %u of %s unpicklable')\n",
                    (unsigned)i,
                    PyString_AsString(canonical_name));
      USER_LOG_PRINTF("CANNOT_MEMOIZE %s | arg %u unpicklable | runtime %ld ms\n",
                      PyString_AsString(canonical_name), (unsigned)i, runtime_ms);
      goto pg_exit_frame_done;
    }

    PyList_Append(stored_args_lst_copy, copy); // this does incref copy
    Py_DECREF(copy); // nullify the increfs to prevent leaks
  }


  PyObject* retval_copy = NULL;

  // if retval can't be pickled, then DON'T MEMOIZE IT 
  //
  if (is_picklable(retval)) {
#ifdef ENABLE_COW
    // defer the deepcopy until elt has been mutated
    retval_copy = retval;
    Py_INCREF(retval_copy); // always incref to match what happens in deepcopy_func
    cow_containment_dict_ADD(retval);
#else
    // eagerly make the deepcopy NOW!
    retval_copy = deepcopy(retval);

    if (!retval_copy) {
      assert(PyErr_Occurred());
      PyErr_Clear();
    }
#endif // ENABLE_COW
  }


  if (retval_copy == NULL) {
    PG_LOG_PRINTF("dict(event='WARNING', what='CANNOT_MEMOIZE', why='Return value of %s unpicklable')\n", 
                  PyString_AsString(canonical_name));
    USER_LOG_PRINTF("CANNOT_MEMOIZE %s | return value unpicklable | runtime %ld ms\n",
                    PyString_AsString(canonical_name), runtime_ms);
    goto pg_exit_frame_done;
  }

  assert(Py_REFCNT(retval_copy) > 0);


  // now memoize results ...

  PyObject* memoized_vals_lst = get_memoized_vals_lst(my_func_memo_info);

  // Note that the return value will always be a list of exactly ONE
  // element, but it's a list to facilitate mutation if you do COW
  // optimization
  PyObject* retval_lst = PyList_New(1);
  Py_INCREF(retval_copy); // ugh, stupid refcounts!
  PyList_SET_ITEM(retval_lst, 0, retval_copy); // this does NOT incref retval_copy

  PyObject* memo_table_entry = PyDict_New();
  PyDict_SetItemString(memo_table_entry, "args", stored_args_lst_copy);
  PyDict_SetItemString(memo_table_entry, "retval", retval_lst);
  Py_DECREF(retval_lst);

  PyObject* runtime_ms_obj = PyLong_FromLong(runtime_ms);
  PyDict_SetItemString(memo_table_entry, "runtime_ms", runtime_ms_obj);
  Py_DECREF(runtime_ms_obj);

  // add OPTIONAL fields of memo_table_entry ...
  if (f->stdout_cStringIO) {
    PyObject* stdout_val = PycStringIO->cgetvalue(f->stdout_cStringIO);
    assert(stdout_val);
    PyDict_SetItemString(memo_table_entry, "stdout_buf", stdout_val);
    Py_DECREF(stdout_val);
  }

  if (f->stderr_cStringIO) {
    PyObject* stderr_val = PycStringIO->cgetvalue(f->stderr_cStringIO);
    assert(stderr_val);
    PyDict_SetItemString(memo_table_entry, "stderr_buf", stderr_val);
    Py_DECREF(stderr_val);
  }

  if (f->globals_read_set) {
    PyObject* global_vars_read = PyDict_New();

    Py_ssize_t s_pos = 0;
    PyObject* global_varname_tuple;
    while (_PySet_Next(f->globals_read_set, &s_pos, &global_varname_tuple)) {
      PyObject* val = find_globally_reachable_obj_by_name(global_varname_tuple, f);

      if (val) {
        deepcopy_and_add_global_read(global_varname_tuple, val, global_vars_read);
      }
      else {
#ifdef ENABLE_DEBUG_LOGGING
        PyObject* tmp_str = PyObject_Repr(global_varname_tuple);
        PG_LOG_PRINTF("dict(event='WARNING', what='global var not found in top_frame->f_globals', varname=\"%s\")\n", PyString_AsString(tmp_str));
        Py_DECREF(tmp_str);
#endif // ENABLE_DEBUG_LOGGING
      }
    }

    PyDict_SetItemString(memo_table_entry, "global_vars_read", global_vars_read);
    Py_DECREF(global_vars_read);
  }

  if (f->files_read_set) {
    PyObject* files_read = PyDict_New();
    Py_ssize_t s_pos = 0;
    PyObject* read_filename;
    while (_PySet_Next(f->files_read_set, &s_pos, &read_filename)) {
      add_file_dependency(read_filename, files_read);
    }
    PyDict_SetItemString(memo_table_entry, "files_read", files_read);
    Py_DECREF(files_read);
  }

  if (f->files_written_set) {
    PyObject* files_written = PyDict_New();
    Py_ssize_t s_pos = 0;
    PyObject* written_filename;
    while (_PySet_Next(f->files_written_set, &s_pos, &written_filename)) {
      add_file_dependency(written_filename, files_written);
    }
    PyDict_SetItemString(memo_table_entry, "files_written", files_written);
    Py_DECREF(files_written);
  }

  // lazy initialize
  if (!memoized_vals_lst) {
    my_func_memo_info->memoized_vals = memoized_vals_lst = PyList_New(0);
  }

  // we're just gonna blindly append memo_table_entry assuming that
  // there are no 'duplicates' in memoized_vals_lst
  //
  // (if this assumption is violated, then we will get some
  // funny-looking results ... but I think I should be able to convince
  // myself of why duplicates should never occur)
  PyList_Append(memoized_vals_lst, memo_table_entry);
  Py_DECREF(memo_table_entry);

  PG_LOG_PRINTF("dict(event='MEMOIZED_RESULTS', what='%s', runtime_ms='%ld')\n",
                PyString_AsString(canonical_name),
                runtime_ms);
  USER_LOG_PRINTF("MEMOIZED %s | runtime %ld ms\n",
                  PyString_AsString(canonical_name),
                  runtime_ms);

  Py_DECREF(retval_copy); // subtle but important for preventing leaks


pg_exit_frame_done:

  Py_XDECREF(stored_args_lst_copy);

  MEMOIZE_PUBLIC_END();
}


// Perform: output_dict[varname] = deepcopy(value)
//
// (punting on values that cannot be safely pickled)
//
//   varname can be munged by find_globally_reachable_obj_by_name()
//   (it's either a string or a tuple of strings)
static void deepcopy_and_add_global_read(PyObject* varname,
                                         PyObject* value,
                                         PyObject* output_dict) {
  // quick-check since a lot of passed-in values are modules,
  // which are NOT picklable ... seems to speed things up slightly
  if (PyModule_CheckExact(value)) {
    return;
  }

  /* don't bother adding objects that don't implement any form of
     non-identity-based comparison (e.g., using __eq__ or __cmp__
     methods), since in those cases, there is no way that we can
     possibly MATCH THEM UP with their original incarnations once we
     load the memoized values from disk (the version loaded from disk
     will be a different object than the one in memory, so '==' will
     ALWAYS FAIL if a comparison method isn't implemented)

     for speed purposes, do this BEFORE checking for is_picklable() since
     has_comparison_method() is much faster! */
  if (!has_comparison_method(value)) {

#ifdef ENABLE_DEBUG_LOGGING
    PyObject* tmp_str = PyObject_Repr(varname);
    char* varname_str = PyString_AsString(tmp_str);
    PG_LOG_PRINTF("dict(event='WARNING', what='UNSOUNDNESS', why='Cannot track global var whose type has no comparison method', varname=\"%s\", type='%s')\n",
                  varname_str, Py_TYPE(value)->tp_name);
    Py_DECREF(tmp_str);
#endif // ENABLE_DEBUG_LOGGING

    return;
  }


  // don't bother storing these values, since they're
  // probably immutable anyhow (and can't be pickled)
  // (this check can be rather slow, so avoid doing it if possible)
  if (!is_picklable(value)) {

#ifdef ENABLE_DEBUG_LOGGING
    // to prevent excessive log noise, only print out a warning 
    // for types that aren't DEFINITELY_NOT_PICKLABLE:
    if (!DEFINITELY_NOT_PICKLABLE(value)) {
      PyObject* tmp_str = PyObject_Repr(varname);
      char* varname_str = PyString_AsString(tmp_str);
      PG_LOG_PRINTF("dict(event='WARNING', what='UNSOUNDNESS', why='Cannot track unpicklable global var', varname=\"%s\", type='%s')\n",
                    varname_str, Py_TYPE(value)->tp_name);
      Py_DECREF(tmp_str);
    }
#endif // ENABLE_DEBUG_LOGGING

    return;
  }


#ifdef ENABLE_COW
  // defer the deepcopy until value has been mutated
  PyObject* global_val_copy = value;
  Py_INCREF(global_val_copy); // always incref to match what happens in deepcopy_func
  cow_containment_dict_ADD(value);

#else
  // deepcopy value before storing it into global_var_dependencies
	PyObject* global_val_copy = deepcopy(value);
#endif // ENABLE_COW

  // if the deepcopy was successful, add it to output_dict
  if (global_val_copy) {
    assert(Py_REFCNT(global_val_copy) > 0);
    PyDict_SetItem(output_dict, varname, global_val_copy); // this DOES incref global_val_copy
    Py_DECREF(global_val_copy); // necessary to prevent leaks
  }
  // Otherwise, if the deepcopy failed, log a warning and punt for now.
  // This is a potential source of unsoundness, since we can't detect 
  // changes in values that we don't track
  //
  // TODO: a conservative alternative is just to mark the entire
  // stack impure whenever an unsoundness warning is issued
  else {
#ifdef ENABLE_DEBUG_LOGGING
    PyObject* tmp_str = PyObject_Repr(varname);
    PG_LOG_PRINTF("dict(event='WARNING', what='UNSOUNDNESS', why='Cannot deepcopy global var', varname='%s', type='%s')\n", PyString_AsString(tmp_str), Py_TYPE(value)->tp_name);
    Py_DECREF(tmp_str);
#endif // ENABLE_DEBUG_LOGGING

    assert(PyErr_Occurred());
    PyErr_Clear();
  }
}

// Perform: output_dict[filename] = last_modification_time(File(filename))
static void add_file_dependency(PyObject* filename, PyObject* output_dict) {
  char* filename_cstr = PyString_AsString(filename);
  FILE* fp = fopen(filename_cstr, "r");

  // it's possible that this file no longer exists (e.g., it was a
  // temp file that already got deleted) ... right now, let's punt on
  // those files, but perhaps there'll be a better future solution:
  if (fp) {
    time_t mtime = PyOS_GetLastModificationTime(filename_cstr, fp);
    fclose(fp);
    assert (mtime >= 0); // -1 is an error value
    PyObject* mtime_obj = PyLong_FromLong((long)mtime);
    PyDict_SetItem(output_dict, filename, mtime_obj);
    Py_DECREF(mtime_obj);
  }
}


static void add_global_read_to_all_frames(PyObject* global_container) {
  assert(global_container);

  PyFrameObject* f = PyEval_GetFrame();
  while (f) {
    if (f->func_memo_info) {
      /* Optimization: When we execute a LOAD of a global or
         globally-reachable value, we simply add its NAME to
         globals_read_set but NOT a dependency on it.  We defer the adding
         of dependencies until the END of a frame's execution.

         Grabbing the global variable values at this time is always safe
         because they are guaranteed to have the SAME VALUES as when they
         were read earlier during this function's invocation.  If the
         values were mutated, then the entire stack would've been marked
         impure anyways, so we wouldn't be memoizing this invocation! */
      LAZY_INIT_SET_ADD(f->globals_read_set, global_container);
    }
    f = f->f_back;
  }
}


// only register this event for loads of globals that are NOT built-ins
// (see Python/ceval.c to make sure this is the case)
void pg_LOAD_GLOBAL_event(PyObject *varname, PyObject *value) {
  /* There is an opportunity for some optimization here, since there are
     lots of LOAD_GLOBALs of boring built-in types and other stuff that
     we don't really care about tracing.

     We NEED to trace modules, since somebody can dive inside a module
     to grab its fields!
  */
  if (PyType_CheckExact(value) ||
      PyCFunction_Check(value) ||
      PyFunction_Check(value) ||
      PyMethod_Check(value) ||
      PyClass_Check(value)) {
    return;
  }

  MEMOIZE_PUBLIC_START()

  PyFrameObject* top_frame = PyEval_GetFrame();
  assert(top_frame);

  PyObject* new_varname = NULL;

  // if the code is ignored, use a special ignore_str as the filename
  // and DO NOT add a global variable dependency
  if (top_frame->f_code->pg_ignore) {
    new_varname = create_varname_tuple(ignore_str, varname);
  }
  // in the regular case, use the filename and also add a global read
  // dependency ...
  else {
    new_varname = create_varname_tuple(top_frame->f_code->co_filename, varname);

    add_global_read_to_all_frames(new_varname);
  }
  assert(new_varname);

  update_global_container_weakref(value, new_varname);

  MEMOIZE_PUBLIC_END()
}

// varname is only used for debugging ...
void pg_STORE_DEL_GLOBAL_event(PyObject *varname) {
  MEMOIZE_PUBLIC_START()

  PyFrameObject* top_frame = PyEval_GetFrame();

  // VERY IMPORTANT - if the function on top of the stack is mutating a
  // global and we're ignoring that function, then we should NOT mark
  // the entire stack impure.  e.g., standard library functions might
  // mutate some global variables, but they are still 'pure' from the
  // point-of-view of client programs
  if (!top_frame || top_frame->f_code->pg_ignore) {
    MEMOIZE_PUBLIC_END() // don't forget me!
    return;
  }

  assert(PyString_CheckExact(varname));

  /* mark all functions currently on stack as impure, since they are
     all executing at the time that a global write occurs */
  PG_LOG_PRINTF("dict(event='SET_GLOBAL_VAR', what='%s')\n",
                PyString_AsString(varname));
  mark_entire_stack_impure("mutate global");

  MEMOIZE_PUBLIC_END()
}


// value is the result object returned from an access of the form object.attrname
void pg_GetAttr_event(PyObject *object, PyObject *attrname, PyObject *value) {
  assert(object);

  // value could be null if obj.attrname is non-existent
  if (!value) return;

  /* Optimization: useless to try to track field accesses of these types:

     We NEED to trace modules, since somebody can dive inside a module
     to grab its fields!
  */
  if (PyType_CheckExact(value) ||
      PyCFunction_Check(value) ||
      PyFunction_Check(value) ||
      PyMethod_Check(value) ||
      PyClass_Check(value)) {
    return;
  }


  MEMOIZE_PUBLIC_START()

  PyObject* global_container = get_global_container(object);
  if (global_container) {
    // If object is a MODULE, then we need to add a more detailed
    // record not just using the module name, but also attach attrname
    // (which is the name of the variable within the module)
    //
    // TODO: do we want to generalize this to when object is something
    // other than a module? (e.g., for foo.bar.baz, where foo is a general
    // object, this would also work)
    if (PyModule_CheckExact(object)) {
      // extend the tuple to include the attribute you're reading:
      PyObject* new_varname = extend_with_attrname(object, attrname);

      // only add a global variable dependency if this value did not
      // originate from a file whose code we want to ignore ...
      if (strcmp(PyString_AsString(PyTuple_GET_ITEM(new_varname, 0)),
                 "IGNORE") != 0) {
        add_global_read_to_all_frames(new_varname);
      }

      update_global_container_weakref(value, new_varname);
    }
    else {
      // extend global reachability to value
      update_global_container_weakref(value, global_container);
    }
  }

  MEMOIZE_PUBLIC_END()
}

// handler for BINARY_SUBSCR opcode: ret = obj[ind]
void pg_BINARY_SUBSCR_event(PyObject* obj, PyObject* ind, PyObject* res) {
  assert(obj);

  // res could be null if obj[ind] is non-existent
  if (!res) return;

  /* Optimization: useless to try to track field accesses of these types: */
  if (PyType_CheckExact(res) ||
      PyCFunction_Check(res) ||
      PyFunction_Check(res) ||
      PyMethod_Check(res) ||
      PyClass_Check(res)) {
    return;
  }


  MEMOIZE_PUBLIC_START()

  // extend global reachability to res
  PyObject* global_container = get_global_container(obj);
  if (global_container) {
    update_global_container_weakref(res, global_container);
  }

  MEMOIZE_PUBLIC_END()
}


// called whenever object is ABOUT TO BE mutated by either storing or
// deleting one of its attributes (e.g., in an instance) or items (e.g.,
// in a sequence)
//
// VERY VERY IMPORTANT that we insert calls to pg_about_to_MUTATE_event
// in places RIGHT BEFORE the mutation actually happens, so that the COW
// optimization can work properly (if we insert the call AFTER the
// mutate happens, then COW will copy the NEW value, not the original
// value, which defeats the whole purpose)
void pg_about_to_MUTATE_event(PyObject *object) {
  MEMOIZE_PUBLIC_START()

  PyFrameObject* top_frame = PyEval_GetFrame();

  // VERY IMPORTANT - if the function on top of the stack is doing some
  // mutation and we're ignoring that function, then we should NOT mark
  // the entire stack impure.  e.g., standard library functions might
  // mutate some global variables, but they are still 'pure' from the
  // point-of-view of client programs
  if (top_frame->f_code->pg_ignore) {
    MEMOIZE_PUBLIC_END() // don't forget me!
    return;
  }


  // OPTIMIZATION: simply checking for global reachability is pretty
  // fast, so do this as the first check (most common case) ...
  PyObject* global_container = get_global_container(object);
  if (global_container) {
    /* SUPER HACK: ignore mutations to global variables defined in files
       whose code we want to ignore (e.g., standard library code)
       Although technically this isn't correct, it's a cheap workaround
       for the fact that some libraries (like re.py) implement global
       caches (see _cache dict in re.py) and actually mutate them when
       doing otherwise pure operations. */
    assert(PyTuple_CheckExact(global_container));
    if (strcmp(PyString_AsString(PyTuple_GET_ITEM(global_container, 0)),
               "IGNORE") == 0) {
      MEMOIZE_PUBLIC_END()
      return;
    }

    mark_entire_stack_impure("mutate global");
  }
  else {
    // then fall back on the more general check, which picks up
    // mutations of function arguments as well:

    // mark each frame as impure if it was called AFTER the object was
    // created (this operationalizes our chosen definition of impurity)
    PyFrameObject* f = top_frame;
    while (f) {
      if (f->func_memo_info) {
        if (get_creation_time(object) < f->start_func_call_time) {
          mark_impure(f, "mutate non-local value");
        }
        else {
          // small optimization: since we are going 'backwards' up the
          // stack, the start_instr_time should be strictly decreasing
          // (for all frames with a func_memo_info), so once we get smaller
          // than get_creation_time(object), we can break out of the loop
          // and not check any more frames
          break;
        }
      }
      f = f->f_back;
    }
  }

#ifdef ENABLE_COW
  // TODO: this occurs on EVERY mutation event, so it might be slow
  check_COW_mutation(object);
#endif

  MEMOIZE_PUBLIC_END()
}


/* initialize self_mutator_c_methods to a trie containing the names of C
   methods that mutate their 'self' parameter */
static void init_self_mutator_c_methods(void) {
  self_mutator_c_methods = TrieCalloc();

  // we want to ignore methods from these common C extension types:
  TrieInsert(self_mutator_c_methods, "append"); // list, bytearray
  TrieInsert(self_mutator_c_methods, "insert"); // list, bytearray
  TrieInsert(self_mutator_c_methods, "extend"); // list, bytearray
  TrieInsert(self_mutator_c_methods, "pop"); // list, dict, set, bytearray
  TrieInsert(self_mutator_c_methods, "remove"); // list, set, bytearray
  TrieInsert(self_mutator_c_methods, "reverse"); // list, bytearray
  TrieInsert(self_mutator_c_methods, "sort"); // list
  TrieInsert(self_mutator_c_methods, "popitem"); // dict
  TrieInsert(self_mutator_c_methods, "update"); // dict, set
  TrieInsert(self_mutator_c_methods, "clear"); // dict, set
  TrieInsert(self_mutator_c_methods, "intersection_update"); // set
  TrieInsert(self_mutator_c_methods, "difference_update"); // set
  TrieInsert(self_mutator_c_methods, "symmetric_difference_update"); // set
  TrieInsert(self_mutator_c_methods, "add"); // set
  TrieInsert(self_mutator_c_methods, "discard"); // set
  TrieInsert(self_mutator_c_methods, "resize"); // numpy.array
}

/* some of these are built-in C functions, while others are Python
   functions.  note that we're currently pretty sloppy since we just do
   matches on function names and not on their module names */
static void init_definitely_impure_funcs(void) {
  definitely_impure_funcs = TrieCalloc();

  TrieInsert(definitely_impure_funcs, "draw"); // matplotlib

  // if you open stdin, then you are impure:
  TrieInsert(definitely_impure_funcs, "input");
  TrieInsert(definitely_impure_funcs, "raw_input");

  // random functions:
  // TODO: let's exclude these for now, since some people actually want
  // a form of determinism when debugging ... and also some library code
  // calls random functions on occasion
  //TrieInsert(definitely_impure_funcs, "random");
  //TrieInsert(definitely_impure_funcs, "randn");
  //TrieInsert(definitely_impure_funcs, "randint");
}


/* Trigger this event when the program is about to call a C extension
   method with a (possibly null) self parameter.  e.g.,

   lst = [1,2,3]
   lst.append(4)  // triggers with func_name as "append" and self as [1,2,3]

   Note that we don't have any more detailed information about the C
   function other than its name, since it's actually straight-up C code,
   so we can't introspectively find out what module it belongs to, etc. */
void pg_about_to_CALL_C_METHOD_WITH_SELF_event(char* func_name, PyObject* self) {
  // note that we don't wrap this function in MEMOIZE_PUBLIC_START and
  // MEMOIZE_PUBLIC_END since we don't plan to call any nested functions
  // and so that pg_about_to_MUTATE_event can properly trigger
  if (!pg_activated) return;


  // first check whether we've called a built-in C function that's in
  // definitely_impure_funcs:
  if (TrieContains(definitely_impure_funcs, func_name)) {
    //USER_LOG_PRINTF("IMPURE C FUNCTION CALL | %s\n", func_name);
    mark_entire_stack_impure(func_name);
    return;
  }


  if (!self) return; // no need to do anything if this is null

  /* ok, what we want to do is match func_name against a list of names
     of methods that are known to mutate their 'self' argument, and if
     there is a match, call pg_about_to_MUTATE_event(self).  This makes
     is so that we don't have to manually edit the library C code to
     explicitly insert calls to pg_about_to_MUTATE_event.

     note that we don't have the module name, so there might be false
     positives.  thankfully, we only do this check for C functions, so
     there won't be name clashes with user-defined Python functions

     for speed, we rely on a trie called self_mutator_c_methods */
  if (TrieContains(self_mutator_c_methods, func_name)) {
    // note that pg_activated must be 1 for this to have any effect ...
    pg_about_to_MUTATE_event(self);
  }
}


// reading from files
static void private_FILE_READ_event(PyFileObject* fobj);

void pg_FILE_OPEN_event(PyFileObject* fobj) {
  if (!fobj) return; // could be null for a failure in opening the file

  MEMOIZE_PUBLIC_START()

  char* mode = PyString_AsString(fobj->f_mode);

  /* A file opened in 'pure-write' mode has a 'w' but NOT 'r', '+', or 'a'
     - these can be tracked and do NOT cause function to be marked as impure

     A file opened in 'mixed-write' mode has a '+' or 'a'
     - we must mark these as impure up-front since the writes aren't
       'self-contained'

   */
  int is_pure_write = 0;
  int is_mixed_write = 0;

  char* pc;
  for (pc = mode; *pc != '\0'; pc++) {
    switch(*pc) {
      case 'w':
        if (!is_mixed_write) {
          is_pure_write = 1;
        }
        break;
      case 'r':
        is_pure_write = 0;
        break;
      case '+':
        is_mixed_write = 1;
        is_pure_write = 0;
        break;
      case 'a':
        is_mixed_write = 1;
        is_pure_write = 0;
        break;
    }
  }

  assert(!(is_mixed_write && is_pure_write));

  if (is_mixed_write) {
    PG_LOG_PRINTF("dict(event='OPEN_FILE_IN_MIXED_WRITE_MODE', what='%s', mode='%s')\n", 
                  PyString_AsString(fobj->f_name), mode);

    mark_entire_stack_impure("opened file in a/+ mode");
  }
  else if (is_pure_write) {
    // add to files_opened_w_set AND files_written_set of all frames on the stack
    // (the reason why we add to files_written_set is that simply
    // opening the file in pure-write mode does a 'write' to it by
    // truncating the file to zero-length)
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_SET_ADD(f->files_opened_w_set, fobj->f_name);
        LAZY_INIT_SET_ADD(f->files_written_set, fobj->f_name);
      }
      f = f->f_back;
    }
  }
  // if you don't trip up any of the other conditions, then it's safe to
  // assume that you opened the file in READ mode
  else {
    // if you open a file in read mode, add a read dependency on it
    // RIGHT AWAY, since some functions (e.g., numpy.fromfile) directly
    // use C library calls to read from the file rather than regular
    // Python function calls that pg_FILE_READ_event can pick up ...
    private_FILE_READ_event(fobj);
  }

  MEMOIZE_PUBLIC_END()
}

void pg_FILE_CLOSE_event(PyFileObject* fobj) {
  if (!fobj) return; // could be null for a failure in opening the file

  MEMOIZE_PUBLIC_START()

  // add to files_closed_set of all frames on the stack
  PyFrameObject* f = PyEval_GetFrame();
  while (f) {
    if (f->func_memo_info) {
      LAZY_INIT_SET_ADD(f->files_closed_set, fobj->f_name);
    }
    f = f->f_back;
  }

  MEMOIZE_PUBLIC_END()
}

/* Current intercepted calls:
     file.read()
     file.readinto()
     file.readline()
     file.readlines()
     file.xreadlines()
     file_iternext() --- for doing "for x in file: ..."

     execution of an sqlite SELECT statement

TODO: how many more are there?  what about native C calls that bypass
Python File objects altogether?
*/
void pg_FILE_READ_event(PyFileObject* fobj) {
  MEMOIZE_PUBLIC_START()
  private_FILE_READ_event(fobj);
  MEMOIZE_PUBLIC_END()
}

// (PRIVATE version, don't call from outside code)
static void private_FILE_READ_event(PyFileObject* fobj) {
  assert(fobj);

  // we are impure if we read from stdin ...
  if (strcmp(PyString_AsString(fobj->f_name), "<stdin>") == 0) {
    mark_entire_stack_impure("read from stdin");
    return;
  }

  PyFrameObject* f = PyEval_GetFrame();
  while (f) {
    if (f->func_memo_info) {
      LAZY_INIT_SET_ADD(f->files_read_set, fobj->f_name);
    }
    f = f->f_back;
  }
}


// writing to files (including stdout/stderr)

static void private_FILE_WRITE_event(PyFileObject* fobj);


void pg_intercept_PyFile_WriteString(const char *s, PyObject *f) {
  MEMOIZE_PUBLIC_START()

  if ((PyObject*)f == PySys_GetObject("stdout")) {
    // log output in stdout_cStringIO for ALL frames on the stack ...

    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stdout_cStringIO);

        // now call the REAL PyFile_WriteString with f->stdout_cStringIO
        // as the parameter in order to 'parrot' the write to this buffer.
        // this SHOULD capture the EXACT same output as written to stdout
        PyFile_WriteString(s, f->stdout_cStringIO);
      }
      f = f->f_back;
    }
  }
  else if ((PyObject*)f == PySys_GetObject("stderr")) {
    // handle stderr in EXACTLY the same way as we handle stdout
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stderr_cStringIO);
        PyFile_WriteString(s, f->stderr_cStringIO);
      }
      f = f->f_back;
    }
  }
  else if (PyFile_Check(f)) { // regular ole' file
    private_FILE_WRITE_event((PyFileObject*)f);
  }
  // else silently ignore

  MEMOIZE_PUBLIC_END()
}

void pg_intercept_PyFile_WriteObject(PyObject *v, PyObject *f, int flags) {
  MEMOIZE_PUBLIC_START()

  if ((PyObject*)f == PySys_GetObject("stdout")) {
    // log output in stdout_cStringIO for ALL frames on the stack ...

    // if a frame doesn't have a func_memo_info (e.g., if it's a
    // top-level module or standard library function), then there's 
    // no way we can memoize it, so don't bother tracking it
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stdout_cStringIO);

        // now call the REAL PyFile_WriteObject with f->stdout_cStringIO
        // as the parameter in order to 'parrot' the write to this buffer.
        // this SHOULD capture the EXACT same output as written to stdout
        PyFile_WriteObject(v, f->stdout_cStringIO, flags);
      }
      f = f->f_back;
    }
  }
  else if ((PyObject*)f == PySys_GetObject("stderr")) {
    // handle stderr in EXACTLY the same way as we handle stdout
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stderr_cStringIO);
        PyFile_WriteObject(v, f->stderr_cStringIO, flags);
      }
      f = f->f_back;
    }
  }
  else if (PyFile_Check(f)) { // regular ole' file
    private_FILE_WRITE_event((PyFileObject*)f);
  }
  // else silently ignore

  MEMOIZE_PUBLIC_END()
}

void pg_intercept_PyFile_SoftSpace(PyObject *f, int newflag) {
  MEMOIZE_PUBLIC_START()

  if ((PyObject*)f == PySys_GetObject("stdout")) {
    // log output in stdout_cStringIO for ALL frames on the stack ...

    // if a frame doesn't have a func_memo_info (e.g., if it's a
    // top-level module or standard library function), then there's 
    // no way we can memoize it, so don't bother tracking it
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stdout_cStringIO);

        // now call the REAL PyFile_SoftSpace with f->stdout_cStringIO
        // as the parameter in order to 'parrot' the write to this buffer.
        // this SHOULD capture the EXACT same output as written to stdout
        PyFile_SoftSpace(f->stdout_cStringIO, newflag);
      }
      f = f->f_back;
    }
  }
  else if ((PyObject*)f == PySys_GetObject("stderr")) {
    // handle stderr in EXACTLY the same way as we handle stdout
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stderr_cStringIO);
        PyFile_SoftSpace(f->stderr_cStringIO, newflag);
      }
      f = f->f_back;
    }
  }
  else if (PyFile_Check(f)) { // regular ole' file
    private_FILE_WRITE_event((PyFileObject*)f);
  }
  // else silently ignore

  MEMOIZE_PUBLIC_END()
}

void pg_intercept_file_write(PyFileObject *f, PyObject *args) {
  MEMOIZE_PUBLIC_START()

  if ((PyObject*)f == PySys_GetObject("stdout")) {
    // log output in stdout_cStringIO for ALL frames on the stack ...

    // if a frame doesn't have a func_memo_info (e.g., if it's a
    // top-level module or standard library function), then there's 
    // no way we can memoize it, so don't bother tracking it
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stdout_cStringIO);

        // now call the REAL file_write with f->stdout_cStringIO
        // as the parameter in order to 'parrot' the write to this buffer.
        // this SHOULD capture the EXACT same output as written to stdout

        // unpack args, which should be a SINGLE string, and pass it into
        // "write" method of stdout_cStringIO:
        assert(PyTuple_Size(args) == 1);
        PyObject* out_string = PyTuple_GetItem(args, 0);
        assert(PyString_CheckExact(out_string));

        char* out_cstr = PyString_AsString(out_string);
        PycStringIO->cwrite(f->stdout_cStringIO, out_cstr, strlen(out_cstr));
      }
      f = f->f_back;
    }
  }
  else if ((PyObject*)f == PySys_GetObject("stderr")) {
    // handle stderr in EXACTLY the same way as we handle stdout
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stderr_cStringIO);
        assert(PyTuple_Size(args) == 1);
        PyObject* out_string = PyTuple_GetItem(args, 0);
        assert(PyString_CheckExact(out_string));

        char* out_cstr = PyString_AsString(out_string);
        PycStringIO->cwrite(f->stderr_cStringIO, out_cstr, strlen(out_cstr));
      }
      f = f->f_back;
    }
  }
  else { // regular ole' file
    private_FILE_WRITE_event(f);
  }

  MEMOIZE_PUBLIC_END()
}

void pg_intercept_file_writelines(PyFileObject *f, PyObject *seq) {
  MEMOIZE_PUBLIC_START()

  if ((PyObject*)f == PySys_GetObject("stdout")) {
    // log output in stdout_cStringIO for ALL frames on the stack ...

    // if a frame doesn't have a func_memo_info (e.g., if it's a
    // top-level module or standard library function), then there's 
    // no way we can memoize it, so don't bother tracking it
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stdout_cStringIO);

        // now call the REAL file_writelines with f->stdout_cStringIO
        // as the parameter in order to 'parrot' the write to this buffer.
        // this SHOULD capture the EXACT same output as written to stdout
        PyObject* method_name = PyString_FromString("writelines");
        PyObject_CallMethodObjArgs(f->stdout_cStringIO, method_name, seq, NULL);
        Py_DECREF(method_name);
      }
      f = f->f_back;
    }
  }
  else if ((PyObject*)f == PySys_GetObject("stderr")) {
    // handle stderr in EXACTLY the same way as we handle stdout
    PyFrameObject* f = PyEval_GetFrame();
    while (f) {
      if (f->func_memo_info) {
        LAZY_INIT_STRINGIO_FIELD(f->stderr_cStringIO);
        PyObject* method_name = PyString_FromString("writelines");
        PyObject_CallMethodObjArgs(f->stderr_cStringIO, method_name, seq, NULL);
        Py_DECREF(method_name);
      }
      f = f->f_back;
    }
  }
  else { // regular ole' file
    private_FILE_WRITE_event(f);
  }

  MEMOIZE_PUBLIC_END()
}

void pg_intercept_file_truncate(PyFileObject *f, PyObject *args) {
  MEMOIZE_PUBLIC_START()

  // I don't think stdout/stderr can be 'truncated' (that seems weird)
  // so I don't need to handle them here
  assert((PyObject*)f != PySys_GetObject("stdout"));
  assert((PyObject*)f != PySys_GetObject("stderr"));

  private_FILE_WRITE_event(f);

  MEMOIZE_PUBLIC_END()
}


static void private_FILE_WRITE_event(PyFileObject* fobj) {
  assert(fobj);

  // add to files_written_set of all frames on the stack
  PyFrameObject* f = PyEval_GetFrame();
  while (f) {
    if (f->func_memo_info) {
      LAZY_INIT_SET_ADD(f->files_written_set, fobj->f_name);
    }
    f = f->f_back;
  }
}

