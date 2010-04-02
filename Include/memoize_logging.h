/* Support code for logging
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_MEMOIZE_LOGGING_H
#define Py_MEMOIZE_LOGGING_H
#ifdef __cplusplus
extern "C" {
#endif


// comment-out to eliminate logging overhead (which isn't that big,
// according to some cursory measurements)
#define ENABLE_LOGGING


#ifdef ENABLE_LOGGING
  #define PG_LOG(str) fprintf(log_file, "%s\n", str);
  #define PG_LOG_PRINTF(...) fprintf (log_file, __VA_ARGS__)

// initialize in pg_initialize(), destroy in pg_finalize()
FILE* log_file;

#else
  #define PG_LOG(str)
  #define PG_LOG_PRINTF(...)
#endif


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_LOGGING_H */

