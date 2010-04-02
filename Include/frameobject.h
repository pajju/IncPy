
/* Frame object interface */

#ifndef Py_FRAMEOBJECT_H
#define Py_FRAMEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


// pgbovine - copied from Include/memoize_profiling.h
#ifdef TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else /* !TIME_WITH_SYS_TIME */
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else /* !HAVE_SYS_TIME_H */
#include <time.h>
#endif /* !HAVE_SYS_TIME_H */
#endif /* !TIME_WITH_SYS_TIME */

#include "memoize_fmi.h" /* pgbovine */

typedef struct {
    int b_type;			/* what kind of block this is */
    int b_handler;		/* where to jump to find handler */
    int b_level;		/* value stack level to pop to */
} PyTryBlock;


// pgbovine - lazily initialize a field like stdout_cStringIO and stderr_cStringIO
#define LAZY_INIT_STRINGIO_FIELD(x) \
  do { \
    if (!x) { \
      PyObject* new_stringIO = PyObject_CallFunction(stringIO_constructor, NULL); \
      assert(new_stringIO); \
      (x) = new_stringIO; \
    } \
  } while (0)



typedef struct _frame {
    PyObject_VAR_HEAD
    struct _frame *f_back;	/* previous frame, or NULL */
    PyCodeObject *f_code;	/* code segment */
    PyObject *f_builtins;	/* builtin symbol table (PyDictObject) */
    PyObject *f_globals;	/* global symbol table (PyDictObject) */
    PyObject *f_locals;		/* local symbol table (any mapping) */
    PyObject **f_valuestack;	/* points after the last local */
    /* Next free slot in f_valuestack.  Frame creation sets to f_valuestack.
       Frame evaluation usually NULLs it, but a frame that yields sets it
       to the current stack top. */
    PyObject **f_stacktop;
    PyObject *f_trace;		/* Trace function */

    /* If an exception is raised in this frame, the next three are used to
     * record the exception info (if any) originally in the thread state.  See
     * comments before set_exc_info() -- it's not obvious.
     * Invariant:  if _type is NULL, then so are _value and _traceback.
     * Desired invariant:  all three are NULL, or all three are non-NULL.  That
     * one isn't currently true, but "should be".
     */
    PyObject *f_exc_type, *f_exc_value, *f_exc_traceback;

    PyThreadState *f_tstate;
    int f_lasti;		/* Last instruction if called */
    /* As of 2.3 f_lineno is only valid when tracing is active (i.e. when
       f_trace is set) -- at other times use PyCode_Addr2Line instead. */
    int f_lineno;		/* Current line number */
    int f_iblock;		/* index in f_blockstack */

    /* pgbovine - this seems like a good place to sneak in some extra
       fields ... probably don't want to sneak them in at the end of the
       struct definition */

    /* BEGIN - pgbovine new fields - don't forget to initialize all of
       them in PyFrame_New() (in Objects/frameobject.c) and also
       deallocate all of them in frame_dealloc() */

    // starting and ending time as timeval structs
    struct timeval start_time;
    struct timeval end_time;

    // the 'time' when this frame started executing
    // (measured in num_executed_instrs)
    unsigned long long int start_instr_time;

    // cStringIO objects representing stdout and stderr output
    // printed by this invocation of f or any of its callees
    // (Optimization: remain NULL when empty, lazy initialize
    // using LAZY_INIT_STRINGIO_FIELD macro)
    PyObject* stdout_cStringIO;
    PyObject* stderr_cStringIO;

    // sets keeping track of file writes by this invocation of f
    // (Optimization: remain NULL when empty)
    PyObject* files_opened_w_set; // opened in WRITE-only mode
    PyObject* files_written_set;
    PyObject* files_closed_set;

    // sets keeping track of global variables READ by this invocation of f
    // (Optimization: remain NULL when empty)
    PyObject* globals_read_set;

    // points to the func_memo_info entry for this frame
    // (is NULL for frames representing top-level modules,
    //  or code that we either can't or don't want to track)
    FuncMemoInfo* func_memo_info;
    /* END   - pgbovine new fields */

    PyTryBlock f_blockstack[CO_MAXBLOCKS]; /* for try and loop blocks */
    PyObject *f_localsplus[1];	/* locals+stack, dynamically sized */
} PyFrameObject;


/* Standard object interface */

PyAPI_DATA(PyTypeObject) PyFrame_Type;

#define PyFrame_Check(op) ((op)->ob_type == &PyFrame_Type)
#define PyFrame_IsRestricted(f) \
	((f)->f_builtins != (f)->f_tstate->interp->builtins)

PyAPI_FUNC(PyFrameObject *) PyFrame_New(PyThreadState *, PyCodeObject *,
                                       PyObject *, PyObject *);


/* The rest of the interface is specific for frame objects */

/* Block management functions */

PyAPI_FUNC(void) PyFrame_BlockSetup(PyFrameObject *, int, int, int);
PyAPI_FUNC(PyTryBlock *) PyFrame_BlockPop(PyFrameObject *);

/* Extend the value stack */

PyAPI_FUNC(PyObject **) PyFrame_ExtendStack(PyFrameObject *, int, int);

/* Conversions between "fast locals" and locals in dictionary */

PyAPI_FUNC(void) PyFrame_LocalsToFast(PyFrameObject *, int);
PyAPI_FUNC(void) PyFrame_FastToLocals(PyFrameObject *);

PyAPI_FUNC(int) PyFrame_ClearFreeList(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_FRAMEOBJECT_H */
