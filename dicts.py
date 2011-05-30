# $Id: dicts.py 6232 2010-03-01 17:16:57Z wheirman $

import time

try:
  from collections import defaultdict
  # Try to derive DDict from collections.defaultdict (only in Python 2.5+)
  # If defaultdict exists, we just need to override __missing__ to implement childArgs, childKwds, init_with_key,
  #   while the most time-consuming part (the normal __getitem__ for non-missing keys) is done in C
  class DDict(defaultdict):
    """Directory with defaults: get of non-existing item initializes item with default value and returns it."""
    def __init__(self, childObject, *childArgs, **childKwds):
      self.childObject = childObject
      self.childArgs = childArgs
      self.childKwds = childKwds
      if 'init_with_key' in childKwds:
        self.init_with_key = childKwds['init_with_key']
        del childKwds['init_with_key']
      else:
        self.init_with_key = False
    def __missing__(self, name):
      if self.init_with_key:
        self[name] = self.childObject(name, *self.childArgs, **self.childKwds)
      else:
        self[name] = self.childObject(*self.childArgs, **self.childKwds)
      return self[name]
except:
  class DDict(dict):
    """Directory with defaults: get of non-existing item initializes item with default value and returns it."""
    def __init__(self, childObject, *childArgs, **childKwds):
      self.childObject = childObject
      self.childArgs = childArgs
      self.childKwds = childKwds
      if 'init_with_key' in childKwds:
        self.init_with_key = childKwds['init_with_key']
        del childKwds['init_with_key']
      else:
        self.init_with_key = False
    def __getitem__(self, name):
      if not name in self:
        if self.init_with_key:
          self[name] = self.childObject(name, *self.childArgs, **self.childKwds)
        else:
          self[name] = self.childObject(*self.childArgs, **self.childKwds)
      return dict.__getitem__(self, name)
