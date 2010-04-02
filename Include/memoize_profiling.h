/* Support code for time profiling
  
   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.
 
   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_MEMOIZE_PROFILING_H
#define Py_MEMOIZE_PROFILING_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"

/*

   Mimics functionality of Modules/timing.h:

   Access fine-grained timing info for rolling my own profiling

  Interface:

  struct timeval before, after;
  BEGIN_TIMING(before);
  END_TIMING(before, after);
  GET_ELAPSED_US(before, after);
  GET_ELAPSED_MS(before, after);
  GET_ELAPSED_S(before, after);

 (Does this only work on UNIX-like systems???)

 */
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

// pass in a struct timeval:
#define BEGIN_TIMING(beforetp) gettimeofday(&beforetp, NULL)

// pass in 'struct timeval' for before AND after, so that it can do
// canonicalization to ease later delta calculations
#define END_TIMING(beforetp, aftertp) gettimeofday(&aftertp, NULL); \
    if(beforetp.tv_usec > aftertp.tv_usec) \
    {  \
         aftertp.tv_usec += 1000000;  \
         aftertp.tv_sec--; \
    }

// get microseconds as 'long int'
#define GET_ELAPSED_US(beforetp, aftertp) (((aftertp.tv_sec - beforetp.tv_sec) * 1000000) + \
		  (aftertp.tv_usec - beforetp.tv_usec))

// get milliseconds as 'long int'
#define GET_ELAPSED_MS(beforetp, aftertp) (((aftertp.tv_sec - beforetp.tv_sec) * 1000) + \
		  ((aftertp.tv_usec - beforetp.tv_usec) / 1000))

// get seconds as 'long int'
#define GET_ELAPSED_S(beforetp, aftertp) ((aftertp.tv_sec - beforetp.tv_sec) + \
		  (aftertp.tv_usec - beforetp.tv_usec) / 1000000)


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_PROFILING_H */

