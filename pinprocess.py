#!/usr/bin/python
# $Id: pinprocess.py 6447 2010-05-18 06:47:40Z wheirman $

import sys, dicts, getopt, mrange, binstore, csv
from libcompat import *

THREADS = 256


class StackRecord:
  def __init__(self, tid, fid, dfid, icount = 0, site = None, parent = None):
    self.tid = tid
    self.dfid = dfid                  # dynamic function id (sequential)
    self.fid = fid                    # static function id (index in F records)
    self.id = (self.tid, self.dfid)
    self.icountStart = icount         # instruction count at start
    self.icountLast = None
    self.site = site or 0             # call site (can be None after jump)
    self.parent = parent              # calling function
    self.region = parent and parent.region or 0
    self.depth = parent and parent.depth + 1 or 1
    self.traceOutput = False          # did we output an E record yet?
    self.name = groupName(self.tid, self.fid, self.dfid, self.region, self.icountStart)
    names[self.id] = self.name
    if doobjects:
      self.ocommr = dicts.DDict(long)   # communication from objects to this function, <object>: <bytecount> dictionary
      self.ocommw = dicts.DDict(long)   # communication from this function to objects, <object>: <bytecount> dictionary
    allFuncs[(tid, dfid)] = self

  def printTrace(self):
    if not self.traceOutput:
      self.traceOutput = True
      if self.parent:
        self.parent.printTrace()
      # output Trace record
      self.printIDs()
      #print ('E', self.tid, self.dfid, self.fid, self.site, self.icountStart)

  def getCallID(self):
    self.printIDs()
    return (self.site, self.fid)

  def printIDs(self):
    global functions, sites
    # if we didn't already, output function description
    if self.fid in functions:
      #print ('F', self.fid, functions[self.fid])
      del functions[self.fid]
    # if we didn't already, output call site description
    if self.site in sites:
      #print ('A', self.site, sites[self.site])
      del sites[self.site]

  def commClean(self):
    # clean up communication matrix: collapse and remove communication to self
    if comm[self.id]:
      for f, bw in comm[self.id].items():
        if f in collapsed:
          comm[self.id][findNonCollapsedParent(f)] += bw
          del comm[self.id][f]
      if self.id in comm[self.id]:
        del comm[self.id][self.id]
    if doobjects:
      for o in self.ocommr.keys():
        o.printTrace()
      for o in self.ocommw.keys():
        o.printTrace()

  def collapse(self):
    collapsed[self.id] = self.parent.id
    for f, bw in comm[self.id].items():
      comm[self.parent.id][f] += bw
    if doobjects:
      for o, bw in self.ocommr.items():
        self.parent.ocommr[o] += bw
      for o, bw in self.ocommw.items():
        self.parent.ocommw[o] += bw

  def exit(self, icount, isCollapsed):
    # if isCollapsed: PinComm trace won't refer to us, we don't need to be remembered
    if isCollapsed and self.dfid:
      del allFuncs[(self.tid, self.dfid)]
    # compute icount
    if icount:
      self.icountLast = icount
    if self.icountLast:
      self.icount = self.icountLast - self.icountStart
    else:
      self.icount = None
    # propagate last icount to parent
    if self.parent:
      self.parent.icountLast = self.icountLast

    # when measuring communication between (dynamic) functions: don't keep all comm entries but process them when the function ends
    if groupby == 'tf':
      # clean up communication matrix
      self.commClean()
      # small functions: collaps into parent
      if self.parent and \
          (   self.icount is None       # not started yet \
           or self.icount < minlen      # short instruction count \
           or (sum(comm[self.id].values()) - comm[self.id].get(self.id, 0) - comm[self.id].get(self.parent.id, 0) < mincomm) \
                                        # low communication \
           or self.fid in libfunctions and self.parent.fid in libfunctions and not insidelibs \
                                        # inside a library function \
          ):
        self.collapse()
      else:
        if started and (mincomm == 0 or comm[self.id]):
          self.printTrace()
        # write out all communication info for this function
        # "C" <thread id> <dynamic function id> \
        #   <communication-to-other-(thread,function)s (not including children)> <instruction count (including children)>
        if comm[self.id]:
          for f, bw in sorted(comm[self.id].items()):
            out.writerow([names[f], self.name, bw])
        if doobjects:
          if self.ocommr:
            pass#print ('OR', self.tid, self.dfid, [ (o.oid, bw) for o, bw in sorted(self.ocommr.items()) ])
          if self.ocommw:
            pass#print ('OW', self.tid, self.dfid, [ (o.oid, bw) for o, bw in sorted(self.ocommw.items()) ])

      if self.traceOutput:
        pass#print ('X', tid, self.dfid, self.icount or 0)

      del comm[self.id]
      if doobjects:
        del self.ocommr
        del self.ocommw

    if not self.parent or self.parent.region != self.region:
      regions[self.region].exit(self.tid, self.icountLast)


def findNonCollapsedParent(f):
  while f in collapsed:
    f = collapsed[f]
  return f



class Object(mrange.Range):

  def __init__(self, oid, tid, returnip, addr, size):
    mrange.Range.__init__(self, addr, addr + size)
    global oidnum, stack, funcid
    if not oid:
      oid = oidnum
      oidnum += 1
    self.oid = oid
    self.size = size
    self.tid = tid
    self.dfid = funcid[tid] and funcid[tid].dfid or 0
    self.traceOutput = False          # did we output an M record yet?
    self.stack = [ f for f in stack[tid] ]
    self.returnip = returnip

  def printTrace(self):
    if not self.traceOutput:
      self.traceOutput = True
      #print ('M', self.oid, self.size, self.tid, self.dfid, self.returnip, [ f.getCallID() for f in self.stack ])



class Region:

  def __init__(self, rid):
    self.rid = rid
    self.start = {}
    self.entries = []

  def mallocgid(self, tid):
    global mallocmerge, groups
    return (groups[tid][0], mallocmerge(self.rid))

  def enter(self, tid, icount):
    self.start[tid] =  icount
    global memsize
    memsize[self.mallocgid(tid)].reset()

  def exit(self, tid, icount):
    if tid not in self.start: self.start[tid] = 0
    stop = icount and (icount - self.start[tid]) or 0
    global memsize
    self.entries.append((tid, self.start[tid], stop, memsize[self.mallocgid(tid)].get()))

  def printTrace(self):
    pass#print ('G', self.rid, self.entries)



class MemSize:

  def __init__(self, gid):
    self.gid = gid
    self.memnow = 0L
    self.memmax = 0L
    self.memmax_tot = 0L
    self.memmin = 0L
    #print (self.gid, 'init')

  def malloc(self, size):
    assert size > 0
    self.memnow += size
    if self.memnow > self.memmax:
      self.memmax = self.memnow
    if self.memnow > self.memmax_tot:
      self.memmax_tot = self.memnow
    #print (self.gid, 'malloc', size, self.memnow, self.memmax, self.memmax_tot)

  def free(self, size):
    assert size > 0
    self.memnow -= size
    if self.memnow < self.memmin:
      self.memmin = self.memnow
    #print (self.gid, 'free', size, self.memnow, self.memmax, self.memmax_tot)

  def reset(self):
    self.memmax = self.memnow
    self.memmin = self.memnow
    #print (self.gid, 'reset', self.memnow, self.memmax, self.memmax_tot)

  def get(self):
    #print (self.gid, 'get', self.memnow, self.memmax, self.memmax_tot)
    return (self.memmin, self.memmax)

  def printTrace(self):
    pass#print ('M', self.gid or (0, 0), self.memmax_tot)

  def __repr__(self):
    return '<MemSize(%s): %d, %d, %d>' % (self.gid, self.memnow, self.memmax, self.memmax_tot)


functions = {}
libfunctions = {}
sites = {}
names = {}
rnames = {}
funcid = [ None for t in xrange(THREADS) ]# current function <dfid> per thread
groups = [ None for t in xrange(THREADS) ]# current groupid per thread
allFuncs = {}
icounts = [ 0 for t in xrange(THREADS) ]  # last icount per thread
oidnum = 0
stack = [ [] for t in xrange(THREADS) ]   # call stack per thread (list of StackRecord objects)
memory = {}                               # (group id (StackRecord.id or aggregate) of last writer to this memory address, dict of all readers that have this location cached)
objects = mrange.RangeDictO(depth = 5, width = 10, size = 1024*1024)
collapsed = {}                            # collapsed[<child>] = <parent> when <child> was short and has been collapsed into <parent>, both StackRecord.id
comm = dicts.DDict(dicts.DDict, long)     # communication between entities
regions = dicts.DDict(Region, init_with_key = True)
mallocs = {}
memsize = dicts.DDict(MemSize, init_with_key = True)
started = False                           # True once we reach the START record


minlen = 0          # minumum length of function (#instructions, including children) for it not to be collapsed into its parent
mincomm = 0         # minumum communication of function (bytes read) for it not to be collapsed into its parent
doobjects = False
insidelibs = False  # if False, collapse all library functions onto the first library call; if True, keep all functions providing a look inside library calls
ignorelibs = []     # patterns to ignore as library functions
groupby = 'tf'
groupby_icount = 0
regionmerge = 'r'
mallocmerge = 'r'
filein = "pincommtrace.pcs"
fileout = "-"


def usage():
  print """\
-i --input    input filename, default is stdin
-o --output   output filename, default is stdout
--minlen      minimum length of functions (#instructions) for functions not to be collapsed into their parent
--objects     enable counting communication per object (malloc range)
--insidelibs  provide view inside library functions, default = collapse each library call and its children into a single node
--ignorelibs  pattern to ignore as library function
--groupby     group functions by: tf (thread, (dynamic) function) [default],
                                  ts (thread, static function),
                                  s  (static function),
                                  tr (thread, region),
                                  r  (region),
                                  t  (thread)
                                  tt:icount  (thread+time, icount = icount (total over all threads) to group time by)
--regionmerge python expression forming a mapping function from regionid `r' to a region identifier, making it possible to merge regions
--mallocmerge same as --regionmerge, but applied on merged regions and only for malloc() counts
"""


try:
  opts, args = getopt.getopt(sys.argv[1:], "ho:i:",
    ["help", "output=", "input=", "minlen=", "mincomm=", "objects", "insidelibs", "ignorelibs=",
     "groupby=", "regionmerge=", "mallocmerge="])
except getopt.GetoptError, e:
  # print help information and exit:
  sys.stderr.write("Incorrect option: %s\n" % e)
  usage()
  sys.exit(2)
for o, a in opts:
  if o in ("-h", "--help"):
    usage()
    sys.exit()
  if o in ("-o", "--output"):
    fileout = a
  if o in ("-i", "--input"):
    filein = a
  if o == "--minlen":
    minlen = long(a)
  if o == "--mincomm":
    mincomm = long(a)
  if o == "--objects":
    doobjects = True
  if o == "--insidelibs":
    insidelibs = True
  if o == "--ignorelibs":
    ignorelibs.append(a)
  if o == "--groupby":
    if ':' in a:
      a, groupby_icount = a.split(':')
      groupby_icount = long(groupby_icount)
    groupby = a
  if o == "--regionmerge":
    regionmerge = a
  if o == "--mallocmerge":
    mallocmerge = a

regionmerge = eval("lambda r: int(" + regionmerge + ")")
mallocmerge = eval("lambda r: int(" + mallocmerge + ")")


def function_name(fid):
  return ':'.join(map(str, functions.get(fid, ['??','??','',0])))

# generate __setGroupId(tid) function.
if groupby == 'tf':
  __groupId = lambda tid, fid, dfid, region, icounts: (tid, dfid)
  groupName = lambda tid, fid, dfid, region, icounts: '%u:%s:%u' % (tid, function_name(fid), dfid)
elif groupby == 'ts':
  __groupId = lambda tid, fid, dfid, region, icounts: (tid, fid)
  groupName = lambda tid, fid, dfid, region, icounts: '%u:%s' % (tid, function_name(fid))
elif groupby == 's':
  __groupId = lambda tid, fid, dfid, region, icounts: (0, fid)
  groupName = lambda tid, fid, dfid, region, icounts: function_name(fid)
elif groupby == 'tr':
  __groupId = lambda tid, fid, dfid, region, icounts: (tid, region)
  groupName = lambda tid, fid, dfid, region, icounts: '%u:%u' % (tid, region)
elif groupby == 'r':
  __groupId = lambda tid, fid, dfid, region, icounts: (0, region)
  groupName = lambda tid, fid, dfid, region, icounts: '%u' % (region)
elif groupby == 't':
  __groupId = lambda tid, fid, dfid, region, icounts: (tid, 0)
  groupName = lambda tid, fid, dfid, region, icounts: '%u' % (tid)
elif groupby == 'tt':
  __groupId = lambda tid, fid, dfid, region, icounts: (tid, int(icounts / groupby_icount))
  groupName = lambda tid, fid, dfid, region, icounts: '%u:%u' % (tid, int(icounts / groupby_icount))
else:
  raise ValueError("Invalid groupby %s" % groupby)


def makeGroupId(tid):
  gid = __groupId(tid, funcid[tid].fid, funcid[tid].dfid, funcid[tid].region, funcid[tid].icountStart)
  rnames[gid] = groupName(tid, funcid[tid].fid, funcid[tid].dfid, funcid[tid].region, funcid[tid].icountStart)
  return gid

def setGroupId(tid):
  """Update the thread's groupid (groups[tid]),
     must be called after each change to funcid[tid] or regions[tid].
  """
  if funcid[tid]:
    groups[tid] = makeGroupId(tid)
  else:
    groups[tid] = ('t' in groupby and tid or 0, -1)



def fEnter(tid, fid, dfid, icount, returnIp = None):
  # thread <tid> is entering new instance of function <fid>
  e = StackRecord(tid, fid, dfid, icount, returnIp, stack[tid] and stack[tid][-1] or None)
  # put on stack
  stack[tid].append(e)
  # set as current
  funcid[tid] = e
  setGroupId(tid)


def fExit(tid, icount = None, isCollapsed = False):
  e = stack[tid].pop()
  e.exit(icount, isCollapsed)
  if stack[tid]:
    funcid[tid] = stack[tid][-1]
  else:
    funcid[tid] = None
  setGroupId(tid)


bs_in = binstore.binload(filein)
# only if opening filein doesn't fail, create output file
out = csv.writer(fileout == '-' and sys.stdout or file(fileout, 'w'))

for args in bs_in:
  #if fidnum[0] > 100000:
  #  print 'done'
  #  sys.stdin.read()
  #  sys.exit(0)

  if args[0] == 'START':
    started = True

  elif args[0] == 'STOP':
    started = False

  elif args[0] == 'END':
    break

  elif args[0] == 'F':
    fid = args[1]
    functions[fid] = args[2:]
    if sum([ args[2].startswith(prefix) for prefix in ('/lib/', '/usr/lib/') ]) and not sum([ args[2].startswith(prefix) for prefix in ignorelibs ]):
      libfunctions[fid] = True

  elif args[0] == 'A':
    sites[args[1]] = args[2:]

  elif args[0] == 'I':
    tid, icount = args[1:]
    icounts[tid] = icount
    if funcid[tid]:
      funcid[tid].icountLast = icount

  elif args[0] == 'S':
    if not started and not doobjects: continue # we only use S records before mallocs (ignored when !doobjects) and just after START
    tid, fids = args[1], args[2:]
    for i in xrange(len(fids), len(stack[tid])):
      fExit(tid)
    for i, (fid, site) in enumerate(fids[:len(stack[tid])]):
      if stack[tid][i].fid != fid:
        for j in xrange(i, len(stack[tid])):
          fExit(tid)
        break
    for fid, site in fids[len(stack[tid]):]:
      fEnter(tid, fid, 0, site)

  elif args[0] == 'E':
    tid, fid, dfid, returnIp, icount = args[1:]
    icounts[tid] = icount
    fEnter(tid, fid, dfid, icount, returnIp)

  elif args[0] == 'X':
    tid, icount, isCollapsed = args[1:]
    icounts[tid] = icount
    # pop <dfid> from stack
    if funcid[tid]: # never pull the last item off the stack
      fExit(tid, icount, isCollapsed)

  elif args[0] == 'J':
    tid, fid, icount = args[1:]
    icounts[tid] = icount
    # look through the stack if we find it
    for i in xrange(len(stack[tid])-1, -1, -1):
      if stack[tid][i].fid == fid:
        for j in xrange(len(stack[tid])-1, i, -1):
          fExit(tid, icount)
        break
    # function was not on the stack, add it now
    if not stack[tid] or stack[tid][-1].fid != fid:
      fEnter(tid, fid, icount)

  elif args[0] == 'M':
    tid, objectid, returnip, addr, size = args[1:]
    if objectid:
      gid = (-1, objectid)
    else:
      gid = groups[tid]
      if gid:
        gid = (gid[0], mallocmerge(gid[1]))
    if doobjects:
      objects.appendUnique(Object(objectid, tid, returnip, addr, size))
    if addr in mallocs:
      _gid, _size = mallocs[addr]
      memsize[_gid].free(_size)
    memsize[gid].malloc(size)
    mallocs[addr] = (gid, size)

  elif args[0] == 'N':
    tid, addr = args[1:]
    if addr in mallocs:
      _gid, _size = mallocs[addr]
      memsize[_gid].free(_size)
      del mallocs[addr]
    elif addr:
      print "unknown free", addr, "!!!!"

  elif args[0] == 'C':
    tid, regionid, dfid, sources = args[1], args[2], args[3], args[4:]
    try:
      e = allFuncs[(tid, dfid)]
    except KeyError:
      e = StackRecord(tid, 0, dfid, 0)
    gid = __groupId(tid, e.fid, dfid, regionid, e.icountStart)
    rnames[gid] = groupName(tid, e.fid, dfid, regionid, e.icountStart)
    for s in sources:
      _tid, _regionid, _dfid, size = s
      try:
        _e = allFuncs[(_tid, _dfid)]
      except KeyError:
        _e = StackRecord(_tid, 0, _dfid, 0)
      _gid = __groupId(_tid, _e.fid, _e.dfid, _regionid, _e.icountStart)
      rnames[_gid] = groupName(_tid, _e.fid, _e.dfid, _regionid, _e.icountStart)
      comm[gid][_gid] += size

  elif args[0] == 'G':
    tid, regionid, icount = args[1:]
    icounts[tid] = icount
    regionid = regionmerge(regionid)
    if funcid[tid] and funcid[tid].region != regionid:
      funcid[tid].region = regionid
      regions[regionid].enter(tid, icount)
      setGroupId(tid)

  elif args[0] == 'T':
    pass

  else:
    raise ValueError("unknown command:", args)


for tid in xrange(THREADS):
  while stack[tid]:
    fExit(tid)


if 's' in groupby:
  for (tid, fid) in comm.keys():
    if fid in functions:
      print ('F', fid, functions[fid])
      del functions[fid]
  for item in comm.values():
    for (tid, fid) in item.keys():
      if fid in functions:
        print ('F', fid, functions[fid])
        del functions[fid]

for toid, value in comm.items():
  if toid:
    for frid, bw in sorted(comm[toid].items()):
      if frid != toid:
        out.writerow([rnames[frid], rnames[toid], bw])

for rid, region in regions.items():
  region.printTrace()

for gid, ms in memsize.items():
  ms.printTrace()
