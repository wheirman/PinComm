/* $Id: binstore.c 6459 2010-05-21 14:54:18Z wheirman $ */

#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include "binstore.h"

BINSTORE * binstore_open(const char * filename, const char * mode)
{
  BINSTORE * bs = (BINSTORE *)malloc(sizeof(BINSTORE));

  if (mode[0] == 'w') {
    if (mode[1] == 'p')
      bs->fp = popen(filename, "w");
    else
      bs->fp = fopen(filename, "wb");
    if (!bs->fp) return NULL;
    bs->gz = gzdopen(fileno(bs->fp), "w9");
    bs->nesting = 0;

  } else if (mode[0] == 'r') {
    if (mode[1] == 'p')
      bs->fp = popen(filename, "r");
    else
      bs->fp = fopen(filename, "rb");
    if (!bs->fp) return NULL;
    bs->gz = gzdopen(fileno(bs->fp), "r");
    #define BUFFER_INITIAL 1048576
    bs->buffer = malloc(BUFFER_INITIAL);
    bs->buffer_size = BUFFER_INITIAL;
    bs->buffer_left = 0;
    bs->ptr = bs->buffer;

  } else
    assert(0);
  return bs;
}

void binstore_close(BINSTORE * bs)
{
  gzclose(bs->gz);
  fclose(bs->fp);
  free(bs);
}

size_t binstore_write(BINSTORE * bs, const void * data, size_t size)
{
  return gzwrite(bs->gz, data, size);
}

void __binstore_store_items(BINSTORE * bs, const char * types, va_list args);

void binstore_store(BINSTORE * bs, const char * types, ...)
{
  va_list args;
  va_start(args, types);
  __binstore_store_items(bs, types, args);
  va_end(args);
  binstore_store_end(bs);
}

void binstore_store_items(BINSTORE * bs, const char * types, ...)
{
  va_list args;
  va_start(args, types);
  __binstore_store_items(bs, types, args);
  va_end(args);
}

void __binstore_store_items(BINSTORE * bs, const char * types, va_list args)
{
  const char * t;
  for(t = types; *t; ++t) {
    binstore_write(bs, t, 1);
    switch(*t) {
      case 'c': {
        char val = va_arg(args, int);
        binstore_write(bs, &val, 1);
        break;
      }
      case 'i': {
        uint32_t val = va_arg(args, uint32_t);
        binstore_write(bs, &val, 4);
        break;
      }
      case 'l': {
        uint64_t val = va_arg(args, uint64_t);
        binstore_write(bs, &val, 8);
        break;
      }
      case 's': {
        const char * c, * val = va_arg(args, const char *);
        long size = strlen(val) + 1;  /* write trailing '\0' character to ease reading */
        for(c = val; *c; ++c)
          assert(*c != '\n');
        binstore_write(bs, &size, 4);
        binstore_write(bs, val, size);
        break;
      }
      case '(':
        ++bs->nesting;
        break;
      case ')':
        --bs->nesting;
        assert(bs->nesting >= 0);
        break;
      default:
        assert(0);
    }
  }
}

void binstore_store_end(BINSTORE * bs)
{
  assert(bs->nesting == 0);
  binstore_write(bs, "\n", 1);
}


/* make sure we can read <bytes> bytes from ptr */
const void * __binstore_read(BINSTORE * bs, size_t bytes)
{
  if (bs->buffer_left >= bytes)
    /* ptr is at least <bytes> bytes before the end of buffer, so nothing to do */
    return bs->ptr;
  /* there's not enough data in the buffer! */
  if (gzeof(bs->gz))
    /* and the file was read completely, so give up */
    return NULL;
  if (bs->buffer_left) {
    /* we're not at the end yet, so copy the remaining data to the beginning */
    if (bs->buffer_left > bs->buffer_size / 2) {
      fprintf(stderr, "Overlapping ranges for memcpy!\n");
      exit(-1);
    }
    memcpy((void *)bs->buffer, bs->ptr, bs->buffer_left);
  }

  /* check if the buffer is large enough */
  if (bytes > bs->buffer_size) {
    bs->buffer_size = (bytes + 0xfff) & ~0xfff;  /* round up to nearest 4KiB */
    bs->buffer = realloc((void *)bs->buffer, bs->buffer_size);
    if (bs->buffer == NULL) {
      fprintf(stderr, "Out of memory!\nCannot allocate `buffer' to %u bytes\n", bs->buffer_size);
      exit(-1);
    }
  }
  bs->ptr = bs->buffer;

  int count = gzread(bs->gz, (void *)bs->ptr + bs->buffer_left, bs->buffer_size - bs->buffer_left);
  assert(count > 0);
  bs->buffer_left += count;

  return bs->buffer_left >= bytes ? bs->ptr : NULL;
}

void binstore_consume(BINSTORE * bs, size_t bytes)
{
  bs->ptr += bytes;
  bs->buffer_left -= bytes;
}

const void * binstore_read(BINSTORE * bs, size_t bytes)
{
  const void * ptr = __binstore_read(bs, bytes);
  binstore_consume(bs, bytes);
  return ptr;
}


/* load one item. returns item type, NULL for end of record, more NULLs for end of file. stores item pointer in *ptr */
char binstore_load(BINSTORE * bs, const void ** ptr)
{
  const char * res = binstore_read(bs, 1);
  if (res == NULL) {
    /* end of file */
    return '\0';
  } else {
    const char type = *res; /* if buffer is reset for reading subsequent data, we loose *res so store type here */
    switch(type) {
      case 'c':
        *ptr = binstore_read(bs, 1);
        break;
      case 'i':
        *ptr = binstore_read(bs, 4);
        break;
      case 'l':
        *ptr = binstore_read(bs, 8);
        break;
      case 's': {
        const uint32_t len = *(uint32_t *)binstore_read(bs, 4);
        *ptr = binstore_read(bs, len);    /* includes trailing '\0' character so *ptr is a normal null-terminated string */
        break;
      }
      case '(':
      case ')':
        break;
      case '\n':
        /* end of record */
        return '\0';
      default:
        assert(0);
    }
    return type;
  }
}
