# $Id: mrange.py 5140 2008-10-12 13:45:53Z wheirman $


class Range:

  def __init__(self, min_ = (), max_ = None):
    self.min = min_
    self.max = max_

  def overlaps(self, value):
    if value is None:
      return False
    elif isinstance(value, Range):
      return self.min < value.max and self.max > value.min
    else:
      return self.min <= value and self.max > value

  def covers(self, value):
    return self.min <= value.min and self.max >= value.max

  def extend(self, value):
    self.min = min(self.min, value.min)
    self.max = max(self.max, value.max)

  def __repr__(self):
    return "Range(%s, %s)" % (self.min, self.max)


class RangeDict:

  def __init__(self):
    self.data = []
    self.range = Range()

  def __contains__(self, key):
    return len(self[key]) > 0

  def __getitem__(self, key):
    return [ item for item in self.data if item.overlaps(key) ]

  def append(self, item):
    self._append(item, unique = False)

  def appendUnique(self, item):
    self._append(item, unique = True)

  def _append(self, item, unique, removeOnly = False):
    self.range.extend(item)
    if unique:
      for i in xrange(len(self.data)-1, -1, -1):
        if item.overlaps(self.data[i]):
          del self.data[i]
    if not removeOnly:
      assert isinstance(item, Range)
      self.data.append(item)

  def __repr__(self):
    return "RangeDict(" + ', '.join([ repr(item) for item in self.data ]) + ")"



class RangeDictO:

  def __init__(self, depth, width, size, threshold = None):
    self.depth = depth
    self.width = width
    self.size = size
    self.threshold = threshold or int(2 * size / width)
    assert self.threshold > 5
    self.parts = {}
    self.data = RangeDict()
    self.range = Range()

  def __contains__(self, key):
    if not self.data and not self.parts: return False
    if not key.overlaps(self.range): return False
    if key in self.data: return True
    for __key in xrange(int(item.min / self.size), int(item.max / self.size) + 1):
      if __key in self.parts:
        if key in self.parts[__key]:
          return True
    return False

  def __getitem__(self, key):
    if not self.data and not self.parts: return []
    if not key.overlaps(self.range): return []
    items = []
    items.extend(self.data[key])
    for __key in xrange(int(key.min / self.size), int(key.max / self.size) + 1):
      if __key in self.parts:
        items.extend(self.parts[__key][key])
    return items

  def append(self, item):
    self._append(item, unique = False)

  def appendUnique(self, item):
    self._append(item, unique = True)

  def _append(self, item, unique, removeOnly = False):
    self.range.extend(item)
    storeLocal = item.max - item.min > self.threshold
    if storeLocal:
      self.data._append(item, unique, removeOnly)
      for __key in xrange(int(item.min / self.size), int(item.max / self.size) + 1):
        if __key in self.parts:
          if unique and item.covers(self.parts[__key].range):
            del self.parts[__key]
          else:
            self.parts[__key]._append(item, unique, removeOnly = True)
    else:
      for __key in xrange(int(item.min / self.size), int(item.max / self.size) + 1):
        if __key not in self.parts:
          if self.depth > 1:
            self.parts[__key] = RangeDictO(self.depth - 1, self.width, self.size / self.width)
          else:
            self.parts[__key] = RangeDict()
        self.parts[__key]._append(item, unique, storeLocal)

  def __repr__(self):
    return "RangeDictO(" + repr(Range(self.min, self.max)) + ", %s, {" % self.size + ", ".join([ "%s:%s" % item for item in self.parts.items() ]) + "})"


if __name__ == '__main__':
  assert not Range(10, 20).contains(9)
  assert     Range(10, 20).contains(10)
  assert     Range(10, 20).contains(19)
  assert not Range(10, 20).contains(20)
  assert not Range(10, 20).contains(21)
  assert not Range(10, 20).contains(Range(0, 10))
  assert     Range(10, 20).contains(Range(5, 15))
  assert     Range(10, 20).contains(Range(15, 25))
  assert     Range(10, 20).contains(Range(19, 30))
  assert not Range(10, 20).contains(Range(20, 30))
  d = RangeDict()
  d.append(Range(10, 20)); d.append(Range(15, 25)); d.append(Range(50, 60))
  assert 0 not in d
  assert 12 in d
  assert len(d[17]) == 2
  r = Range(17, 18)
  d.appendUnique(r)
  assert d[17] == [ r ]
