# visualize pickles produced by IncPy

# pass in a directory (argv[1]) that contains an incpy-cache/ sub-directory

import os, sys, re
import cPickle
import pprint
import hashlib

def render_memo_table_entry_lst(memo_table_lst):
  for e in memo_table_lst:
    canonical_name = e['canonical_name']
    print '===', canonical_name, e['runtime_ms'], 'ms ==='
    print '  Args:', e['args']

    try:
      global_var_dependencies = e['global_vars_read']
      print '  Globals:'
      for global_var in sorted(global_var_dependencies.keys()):
        print '    ', global_var, ':', repr(global_var_dependencies[global_var])
    except KeyError:
      pass

    retval = e['retval']
    print '  Return value:', retval
    print

    code_deps = e['code_dependencies']
    if len(code_deps) > 1:
      print '  Called functions:'
      for func_name in sorted(code_deps):
        if func_name != canonical_name:
          print '   ', func_name
      print

    if 'stdout_buf' in e:
      stdout_repr = repr(e['stdout_buf'])
      if len(stdout_repr) > 50:
        print '  stdout_buf:', stdout_repr[:50], '< %d total chars >' % len(stdout_repr)
      else:
        print '  stdout_buf:', stdout_repr
    if 'stderr_buf' in e:
      stderr_repr = repr(e['stderr_buf'])
      if len(stderr_repr) > 50:
        print '  stderr_buf:', stderr_repr[:50], '< %d total chars >' % len(stderr_repr)
      else:
        print '  stderr_buf:', stderr_repr
    try:
      file_read_dependencies = e['files_read']
      print '  Files read:', ','.join(sorted(file_read_dependencies.keys()))
    except KeyError:
      pass

    try:
      final_file_seek_pos = e['final_file_seek_pos']
      print '  Final seek position for read files:', final_file_seek_pos
    except KeyError:
      pass

    try:
      file_write_dependencies = e['files_written']
      print '  Files written:', ','.join(sorted(file_write_dependencies.keys()))
    except KeyError:
      pass


def main(argv=None):
  if not argv: argv = sys.argv
  dirname = argv[1]
  assert os.path.isdir(dirname)

  incpy_cache_dir = os.path.join(dirname, 'incpy-cache')
  function_cache_dirs = [e for e in os.listdir(incpy_cache_dir) if e.endswith('.cache')]

  for d in function_cache_dirs:
    basename = d.split('.')[0]
    cache_dir_path = os.path.join(incpy_cache_dir, d)
    assert os.path.isdir(cache_dir_path)
    for memo_table_entry_pickle in os.listdir(cache_dir_path):
      p = os.path.join(cache_dir_path, memo_table_entry_pickle)
      memo_table_entry_lst = cPickle.load(open(p))
      render_memo_table_entry_lst(memo_table_entry_lst)

  return 0


if __name__ == "__main__":
  sys.exit(main())

"""
import viz_pickles; from viz_pickles import *
reload(viz_pickles); from viz_pickles import *
"""
