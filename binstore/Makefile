# $Id: Makefile 6458 2010-05-21 14:53:41Z wheirman $

CFLAGS = -g -Wall
CC = gcc

all : libbinstore.a _binstore.so

%.o : %.c *.h Makefile
	gcc -c -fPIC $(CFLAGS) $< -o $@

libbinstore.a : binstore.o
	ar rcs $@ $+

binstoremodule.o : binstoremodule.c *.h Makefile
	gcc -fPIC -I`python -c 'import sys;print "%s/include/python%u.%u" % (sys.prefix, sys.version_info[0], sys.version_info[1])'` $(CFLAGS) -c binstoremodule.c

_binstore.so : binstoremodule.o libbinstore.a
	gcc -shared $< -lz -L. -lbinstore -o $@

clean :
	rm -f *.o *.a *.so
