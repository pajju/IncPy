/* Support code for pickling
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#include "memoize.h"
#include "memoize_pickle.h"


// This is a potentially SLOW (but always SOUND) method, since it
// actually tries to do pickling, which can take a LONG time for a
// large and/or complex object
static int _is_picklable_SLOW_BUT_SOUND(PyObject* obj) {
  // just try to pickle it to a string (we'll just throw away the
  // result), and if there's an error, then it's unpicklable
  PyObject* tup = PyTuple_Pack(1, obj);
  PyObject* ret = PyObject_Call(cPickle_dumpstr_func, tup, NULL);
  Py_DECREF(tup);
  if (!ret) {
    assert(PyErr_Occurred());
    PyErr_Clear();
    return 0;
  }
  else {
    Py_DECREF(ret);
    return 1;
  }
}


#define MAX_DEPTH 6

// hopefully much faster than _is_picklable_SLOW_BUT_SOUND, but could be
// unsound since it uses heuristics that assume collections have a
// uniform type ...
//
// if depth_level surpasses MAX_DEPTH, then simply try to pickle obj to
// see whether it's picklable (we do this to prevent the possibility of
// infinite recursion when there are circular references)
static int _is_picklable_FAST_BUT_UNSOUND(PyObject* obj, int depth_level) {
  
  // the FASTEST path ... if it's a primitive picklable type, then we
  // simply say YES!
  if (DEFINITELY_PICKLABLE(obj)) {
    return 1;
  }

  // if it's untraceable, then definitely NO!
  if (DEFINITELY_NOT_PICKLABLE(obj)) {
    return 0;
  }


  // slow path, but we must do it as a last resort ...
  if (depth_level > MAX_DEPTH) {
    return _is_picklable_SLOW_BUT_SOUND(obj);
  }

  /*

   if it's a built-in collection type (list, tuple, dict, set), then
   simply grab the FIRST element and RECURSE on that.  this heuristic is
   sound under the assumption that ALL elements in a collection have the
   same type, which is often true in practice but obviously NOT
   guaranteed (because some jackass could always stuff in an unpicklable
   element to ruin our day)

   */
  if (PyList_Check(obj)) {
    if (PyList_Size(obj) == 0) { // empty list is always fine to pickle
      return 1;
    }
    else {
      return _is_picklable_FAST_BUT_UNSOUND(PyList_GetItem(obj, 0),
                                            depth_level + 1);
    }
  }
  else if (PyTuple_Check(obj)) {
    if (PyTuple_Size(obj) == 0) { // empty tuple is always fine to pickle
      return 1;
    }
    else {
      return _is_picklable_FAST_BUT_UNSOUND(PyTuple_GetItem(obj, 0),
                                            depth_level + 1);
    }
  }
  else if (PySet_Check(obj)) {
    if (PySet_Size(obj) == 0) { // empty set is always fine to pickle
      return 1;
    }
    else {
      Py_ssize_t pos = 0;
      PyObject* child;
      long hash;
      // just grab the 'first' element and then break
      while (_PySet_NextEntry(obj, &pos, &child, &hash)) {
        return _is_picklable_FAST_BUT_UNSOUND(child,
                                              depth_level + 1);
      }
    }
  }
  else if (PyDict_Check(obj)) {
    if (PyDict_Size(obj) == 0) { // empty dict is always fine to pickle
      return 1;
    }
    else {
      PyObject* key = NULL;
      PyObject* value = NULL;
      Py_ssize_t pos = 0;
      // grab the 'first' key AND value pair and then 'AND' them together,
      // since BOTH need to be picklable
      while (PyDict_Next(obj, &pos, &key, &value)) {
        int key_is_picklable = _is_picklable_FAST_BUT_UNSOUND(key,
                                                              depth_level + 1);
        int val_is_picklable = _is_picklable_FAST_BUT_UNSOUND(value,
                                                              depth_level + 1);
        return (key_is_picklable && val_is_picklable);
      }
    }
  }
  else {
    // SLOW path as last resort ...
    return _is_picklable_SLOW_BUT_SOUND(obj);

    /*

     TODO: all OBJECT instances will defer to this slow path, even if
     they contain collections within them ... this might become an
     issue when we try to work with scientific libraries that use
     matrix objects or whatever

     Jiwon suggests:

       "One heuristic can be..., if it's an instance of user created
       class, then if it doesn't implement reduce or reduce_ex function
       it cannot be pickled (I guess)"

     */
  }

  assert(0); // should never reach here
}


// try to do this as FAST as possible ... but potentially SLOW if you
// have to fall back to _is_picklable_SLOW_BUT_SOUND, ugh :/
int is_picklable(PyObject* obj) {
  // use the fast OPTIMIZED version!!!
  // always start with level 1 ...
  return _is_picklable_FAST_BUT_UNSOUND(obj, 1);

  // slower version, for doing performance comparison:
  //return _is_picklable_SLOW_BUT_SOUND(obj);
}

