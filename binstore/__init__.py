# $Id: __init__.py 5177 2008-11-03 09:12:19Z wheirman $

from binstore._binstore import *

if __name__ == '__main__':
  import sys
  for f in len(sys.argv) > 1 and sys.argv[1:] or '-':
    bs = binload(f)
    for r in bs:
      print r
