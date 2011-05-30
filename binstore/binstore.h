/* $Id: binstore.h 5107 2008-09-29 15:18:02Z wheirman $ */

#ifndef BINSTORE_H
#define BINSTORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <zlib.h>
#include "binstore.h"

typedef struct {
  FILE * fp;
  gzFile gz;
  /* write */
  int nesting;
  /* read */
  const void * buffer;
  const void * ptr;
  size_t buffer_size;
  size_t buffer_left;
} BINSTORE;

BINSTORE * binstore_open(const char * filename, const char * mode);
void binstore_close(BINSTORE * bs);
void binstore_store(BINSTORE * bs, const char * types, ...);
void binstore_store_items(BINSTORE * bs, const char * types, ...);
void binstore_store_end(BINSTORE * bs);
char binstore_load(BINSTORE * bs, const void ** ptr);

#ifdef __cplusplus
}
#endif

#endif // BINSTORE_H
