# visualize pickles produced by IncPy

# pass in a directory (argv[1]) that contains an incpy-cache/ sub-directory

import os, sys, re
import cPickle
import pprint


# render one func_memo_info 'object', which is a dict
# (side effect: DESTROYS fmi in the process of rendering it)
def render_func_memo_info(fmi):
  all_keys = set(fmi.keys())

  print '==='
  canonical_name = fmi.pop('canonical_name')
  print canonical_name
  print '---'

  try:
    memoized_vals = fmi.pop('memoized_vals')
    if memoized_vals:
      print 'Memoized values:'
      for e in memoized_vals:
        retval = e['retval']
        assert len(retval) == 1 # retval is formatted as a singleton list
        retval = retval[0]
        print ' ', e['args'], '->', retval,' (', e['runtime_ms'], 'ms )'
        if 'stdout_buf' in e:
          print '  stdout_buf:', repr(e['stdout_buf'])
        if 'stderr_buf' in e:
          print '  stderr_buf:', repr(e['stderr_buf'])

  except KeyError:
    print 'NO memoized values'
  print

  try:
    file_read_dependencies = fmi.pop('file_read_dependencies')
    if file_read_dependencies:
      print 'Files read:', ','.join(sorted(file_read_dependencies.keys()))
      print
  except KeyError:
    pass

  try:
    file_write_dependencies = fmi.pop('file_write_dependencies')
    if file_write_dependencies:
      print 'Files written:', ','.join(sorted(file_write_dependencies.keys()))
      print
  except KeyError:
    pass

  try:
    global_var_dependencies = fmi.pop('global_var_dependencies')
    if global_var_dependencies:
      print 'Global vars read:'
      for global_var in sorted(global_var_dependencies.keys()):
        print ' ', global_var, ':', repr(global_var_dependencies[global_var])
      print
  except KeyError:
    pass

  try:
    code_dependencies = fmi.pop('code_dependencies')
    if code_dependencies:
      print 'Code dependencies:'
      for func_name in sorted(code_dependencies.keys()):
        print ' ', func_name
      print
  except KeyError:
    pass

  # make sure we've rendered them all!
  assert len(fmi) == 0


def main(argv=None):
  if not argv: argv = sys.argv
  dirname = argv[1]
  filenames_file = os.path.join(dirname, 'incpy-cache/filenames.pickle')
  assert os.path.isfile(filenames_file), filenames_file
  filenames_dict = cPickle.load(open(filenames_file))
  func_names = sorted(filenames_dict.keys())
  for func in func_names:
    v = filenames_dict[func]
    func_filename = os.path.join(dirname, v)
    assert os.path.isfile(func_filename)
    try:
      func_dict = cPickle.load(open(func_filename))
    except:
      print "!!! Error loading pickle file for", func
      continue
    assert func == func_dict['canonical_name']
    render_func_memo_info(func_dict)
    del func_dict
    print

  return 0


if __name__ == "__main__":
  sys.exit(main())

"""
import viz_pickles; from viz_pickles import *
reload(viz_pickles); from viz_pickles import *
"""
