# summarizes the contents of a directory's incpy-cache/ sub-directory
#
# pass in a directory (argv[1]) that contains an incpy-cache/ sub-directory

import os, sys, stat
import cPickle

if __name__ == "__main__":
  dirname = sys.argv[1]
  assert os.path.isdir(dirname)

  incpy_cache_dir = os.path.join(dirname, 'incpy-cache')
  function_cache_dirs = [e for e in os.listdir(incpy_cache_dir) if e.endswith('.cache')]

  print 'incpy-cache/'
  for d in function_cache_dirs:
    basename = d.split('.')[0]
    print ' ', d + '/'
    cache_dir_path = os.path.join(incpy_cache_dir, d)
    assert os.path.isdir(cache_dir_path)
    pickle_files = [e for e in os.listdir(cache_dir_path) if e.endswith('.pickle')]
    if pickle_files:
      p = os.path.join(cache_dir_path, pickle_files[0])
      memo_table_entry_lst = cPickle.load(open(p))
      if memo_table_entry_lst:
        print '    Function/filename:', memo_table_entry_lst[0]['canonical_name']
        print '    Num. cache entries:', len(pickle_files)

        filesizes = [os.stat(os.path.join(cache_dir_path, e))[stat.ST_SIZE] for e in pickle_files]
        total_size = sum(filesizes)
        avg_size = total_size / len(filesizes)
        print '    Average size per entry: %d bytes' % avg_size
        print '    Total size: %d bytes' % total_size
    print

