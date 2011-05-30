/* $Id: pinmagic.h 6243 2010-03-03 10:11:53Z wheirman $ */

#ifndef PINMAGIC_H
#define PINMAGIC_H


#define __PIN_MAGIC(n) do {                                         \
        __asm__ __volatile__ ("movl %0, %%eax;                      \
                               xchg %%bx,%%bx"                      \
                               : /* no output registers */          \
                               : "r" (n) /* input register */       \
                               : "%eax"  /* clobbered register */   \
                              );                                    \
} while (0)

#define __PIN_MAGIC3(n, a1, a2) do {                                \
        __asm__ __volatile__ ("movl %0, %%eax;                      \
                               movl %1, %%ecx;                      \
                               movl %2, %%edx;                      \
                               xchg %%bx,%%bx"                      \
                               : /* no output registers */          \
                               : "r" (n), "r" (a1), "r" (a2)   /* input register */       \
                               : "%eax", "%ecx", "%edx"        /* clobbered register */   \
                              );                                    \
} while (0)

#define __PIN_CMD_MASK              0xff000000
#define __PIN_CMD_OFFSET            24
#define __PIN_ID_MASK               (~(__PIN_CMD_MASK))

#define __PIN_MAGIC_SIMICS          0
#define __PIN_MAGIC_START           1
#define __PIN_MAGIC_STOP            2
#define __PIN_MAGIC_END             3

#define __PIN_MAGIC_CMD_NOARG       0
#define __PIN_MAGIC_REGION          1
#define __PIN_MAGIC_MALLOC          2     /* track next malloc as object id <arg> */
#define __PIN_MAGIC_MALLOCM         3     /* track memory range <arg1> .. <arg1>+<arg2> as object id <arg> */
#define __PIN_MAGIC_ZONE_ENTER      0x04
#define __PIN_MAGIC_ZONE_EXIT       0x05

#define __PIN_MAKE_CMD_ARG(cmd, arg) ((cmd) << __PIN_CMD_OFFSET | ((arg) & __PIN_ID_MASK))
#define __PIN_CMD_ARG(cmd, arg)     __PIN_MAGIC(__PIN_MAKE_CMD_ARG(cmd, arg))


#define PIN_START()                 __PIN_MAGIC(__PIN_MAGIC_START)
#define PIN_STOP()                  __PIN_MAGIC(__PIN_MAGIC_STOP)

#define PIN_SIMICS()                __PIN_MAGIC(__PIN_MAGIC_SIMICS)

#define PIN_MALLOC(oid)             __PIN_CMD_ARG(__PIN_MAGIC_MALLOC, oid)

#define PIN_REGION(rid)             __PIN_CMD_ARG(__PIN_MAGIC_REGION, rid)

#define PIN_ZONE_ENTER(rid)         __PIN_CMD_ARG(__PIN_MAGIC_ZONE_ENTER, rid)
#define PIN_ZONE_EXIT(rid)          __PIN_CMD_ARG(__PIN_MAGIC_ZONE_EXIT, rid)



/************
  Test to override new and delete to tracking functions
    only works when new/delete are actually used, not when objects are allocated statically or
    as part of another object.

#include <malloc.h>
#define PIN_TRACK_OBJECT(oid)       static void* operator new (size_t size) {     \
                                      PIN_MALLOC(oid);                            \
                                      return malloc(size);                        \
                                    }                                             \
                                    static void operator delete (void *p) {       \
                                      free(p);                                    \
                                    }

************/


#define PIN_TRACK(oid, addr, size)  __PIN_MAGIC3(__PIN_MAKE_CMD_ARG(__PIN_MAGIC_MALLOCM, oid), addr, size)
#define PIN_TRACK_OBJECT(oid, obj)  PIN_TRACK(oid, obj, sizeof(*obj))
#define PIN_TRACK_THIS(oid)         PIN_TRACK(oid, this, sizeof(*this))


#endif // PINMAGIC_H
