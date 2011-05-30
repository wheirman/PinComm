# $Id: libcompat.py 5483 2009-02-26 12:54:37Z wheirman $

# Implement some builtins (set and sorted so far) of Python 2.4+, for use on hydra nodes with Python 2.3/4
# Include using:
# from libcompat import *
# set, sorted will then be avaliable either native on Python 2.4+, or emulated on older Pythons

try:
  set = set
except:
  # set not builtin in Python <2.4
  import sets
  set = sets.Set

try:
  sorted = sorted
except:
  # sorted() not implemented on Python <2.4
  def sorted(l, *args, **kwds):
    l = list(l)
    l.sort(*args, **kwds)
    return l
