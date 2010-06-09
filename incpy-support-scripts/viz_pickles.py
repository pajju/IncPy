# visualize pickles produced by IncPy

# pass in a directory (argv[1]) that contains an incpy-cache/ sub-directory

import os, sys, re
import cPickle
import pprint
import hashlib

# render a dependencies 'object', which is a dict
def render_dependencies(fmi):
  canonical_name = fmi['canonical_name']
  print '===', canonical_name, '==='
  print

  code_deps = fmi['code_dependencies']
  if len(code_deps) > 1:
    print 'Called functions:'
    for func_name in sorted(code_deps):
      if func_name != canonical_name:
        print ' ', func_name
    print


# render one memoized_vals 'object', which is a dict
def render_memoized_vals(memoized_vals):
  print 'Memoized vals:'
  for (i, e) in enumerate(memoized_vals):
    print
    print '  ~~~', e['runtime_ms'], 'ms ~~~'
    print '  Args:', e['args']

    try:
      global_var_dependencies = e['global_vars_read']
      print '  Globals:'
      for global_var in sorted(global_var_dependencies.keys()):
        print '    ', global_var, ':', repr(global_var_dependencies[global_var])
    except KeyError:
      pass

    retval = e['retval']
    assert len(retval) == 1 # retval is formatted as a singleton list
    retval = retval[0]
    print '  Return value:', retval

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
    print
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
    print

  return 0


if __name__ == "__main__":
  sys.exit(main())

"""
import viz_pickles; from viz_pickles import *
reload(viz_pickles); from viz_pickles import *
"""
