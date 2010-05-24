# visualize pickles produced by IncPy

# pass in a directory (argv[1]) that contains an incpy-cache/ sub-directory

import os, sys, re
import cPickle
import pprint
import hashlib

# render a dependencies 'object', which is a dict
# (side effect: DESTROYS fmi in the process of rendering it)
def render_dependencies(fmi):
  all_keys = set(fmi.keys())

  canonical_name = fmi.pop('canonical_name')
  print '===', canonical_name, '==='
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
    called_funcs_set = fmi.pop('called_funcs_set')
    if called_funcs_set:
      print 'Called functions:'
      for func_name in sorted(called_funcs_set):
        print ' ', func_name
      print
  except KeyError:
    pass

  assert fmi.pop('self_code_dependency') # pop this but don't bother rendering it

  # make sure we've rendered them all!
  assert len(fmi) == 0


# render one memoized_vals 'object', which is a dict
# (side effect: DESTROYS fmi in the process of rendering it)
def render_memoized_vals(memoized_vals):
  print 'Memoized vals:'
  for e in memoized_vals:
    retval = e['retval']
    assert len(retval) == 1 # retval is formatted as a singleton list
    retval = retval[0]
    print ' ', e['args'], '->', retval,' (', e['runtime_ms'], 'ms )'
    if 'stdout_buf' in e:
      print '  stdout_buf:', repr(e['stdout_buf'])
    if 'stderr_buf' in e:
      print '  stderr_buf:', repr(e['stderr_buf'])


def main(argv=None):
  if not argv: argv = sys.argv
  dirname = argv[1]
  assert os.path.isdir(dirname)

  incpy_cache_dir = os.path.join(dirname, 'incpy-cache')
  files_with_memoized_vals = [e for e in os.listdir(incpy_cache_dir) if 'memoized_vals' in e]

  for f in files_with_memoized_vals:
    (base, mv, pk) = f.split('.')
    assert mv == 'memoized_vals'
    assert pk == 'pickle'
    deps_filename = base + '.dependencies.pickle'
    assert os.path.isfile(os.path.join(incpy_cache_dir, deps_filename))
    deps = cPickle.load(open(os.path.join(incpy_cache_dir, deps_filename)))
    memoized_vals = cPickle.load(open(os.path.join(incpy_cache_dir, f)))
    render_dependencies(deps)
    render_memoized_vals(memoized_vals)
    print '---'

  return 0


if __name__ == "__main__":
  sys.exit(main())

"""
import viz_pickles; from viz_pickles import *
reload(viz_pickles); from viz_pickles import *
"""
