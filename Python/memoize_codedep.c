/* Serializable code dependency 'class'
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#include "memoize.h"
#include "memoize_codedep.h"


// initialize in pg_initialize(), destroy in pg_finalize()
PyObject* module_str = NULL;

/*

Code dependency objects are just like Python code objects, except that
they are picklable (that is, they only take the fields in a code object
that are picklable).

They capture all aspects of the bytecode that are necessary for
detecting whether someone has changed it (co_code isn't sufficient, need
a bunch of other stuff like co_consts). the reason why we use this and
not simply the code object is that the code object can't be pickled :(

'''
From: http://docs.python.org/reference/datamodel.html

Special read-only attributes: co_name gives the function name;
co_argcount is the number of positional arguments (including arguments
with default values); co_nlocals is the number of local variables used
by the function (including arguments); co_varnames is a tuple containing
the names of the local variables (starting with the argument names);
co_cellvars is a tuple containing the names of local variables that are
referenced by nested functions; co_freevars is a tuple containing the
names of free variables; co_code is a string representing the sequence
of bytecode instructions; co_consts is a tuple containing the literals
used by the bytecode; co_names is a tuple containing the names used by
the bytecode; co_filename is the filename from which the code was
compiled; co_firstlineno is the first line number of the function;
co_lnotab is a string encoding the mapping from bytecode offsets to line
numbers (for details see the source code of the interpreter);
co_stacksize is the required stack size (including local variables);
co_flags is an integer encoding a number of flags for the interpreter.
'''

*/

// constructor:
PyObject* CREATE_NEW_code_dependency(PyCodeObject* codeobj) {
  // n = {}
  PyObject* n = PyDict_New();

  PyObject* tmp;

  // copy over all possibly relevant fields from codeobj
  // TODO: are there ones we can safely cut out for efficiency reasons later?
  PyDict_SetItemString(n, "canonical_name", codeobj->pg_canonical_name);

  tmp = PyInt_FromLong(codeobj->co_argcount);
  PyDict_SetItemString(n, "co_argcount", tmp);
  Py_DECREF(tmp);

  tmp = PyInt_FromLong(codeobj->co_nlocals);
  PyDict_SetItemString(n, "co_nlocals", tmp);
  Py_DECREF(tmp);
  
  tmp = PyInt_FromLong(codeobj->co_stacksize);
  PyDict_SetItemString(n, "co_stacksize", tmp);
  Py_DECREF(tmp);

  tmp = PyInt_FromLong(codeobj->co_flags);
  PyDict_SetItemString(n, "co_flags", tmp);
  Py_DECREF(tmp);

  PyDict_SetItemString(n, "co_code", codeobj->co_code);
  PyDict_SetItemString(n, "co_names", codeobj->co_names);
  PyDict_SetItemString(n, "co_varnames", codeobj->co_varnames);
  PyDict_SetItemString(n, "co_freevars", codeobj->co_freevars);
  PyDict_SetItemString(n, "co_cellvars", codeobj->co_cellvars);


  // ok, codeobj->co_consts poses a problem, since sometimes there are 
  // CODE OBJECTS within co_consts (e.g., a module will often contain code
  // objects for its included functions as constants, such as lambdas!!!)
  //
  // The RIGHT solution is to replace each code object with a new
  // code_dependency object that serves as its picklable proxy
  PyObject* new_co_consts = PyList_New(0);

  PyObject *iterator = PyObject_GetIter(codeobj->co_consts);
  assert(iterator);

  PyObject *item;
  while ((item = PyIter_Next(iterator))) {
    // create a picklable code_dependency object as a proxy:
    if (PyCode_Check(item)) {
      // i hope that this recursive call to CREATE_NEW_code_dependency
      // always bottoms out ;)
      PyObject* tmp = CREATE_NEW_code_dependency((PyCodeObject*)item);
      PyList_Append(new_co_consts, tmp);
      Py_DECREF(tmp);
    }
    else {
      PyList_Append(new_co_consts, item);
    }

    /* release reference when done */
    Py_DECREF(item);
  }
  Py_DECREF(iterator);

  assert(PyList_Size(new_co_consts) == PyTuple_Size(codeobj->co_consts));

  // convert list into tuple for consistency in storage:
  tmp = PyList_AsTuple(new_co_consts);
  PyDict_SetItemString(n, "co_consts", tmp);
  Py_DECREF(tmp);

  Py_DECREF(new_co_consts);

  return n;
}


int code_dependency_EQ(PyObject* codedep1, PyObject* codedep2) {
  // code_dependency 'objects' are dicts
  assert(PyDict_CheckExact(codedep1));
  assert(PyDict_CheckExact(codedep2));
  return obj_equals(codedep1, codedep2);
}

