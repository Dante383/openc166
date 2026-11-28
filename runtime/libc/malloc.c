/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

/* First fit from a list of free blocks in address order, merged with their
   neighbours when freed. A block starts with its size (header included);
   a free one also has the next free block. */
extern char __c166_heap[];
extern const size_t __c166_heap_size;

typedef struct block {
  size_t size;
  struct block *next;
} block;
#define HEADER sizeof(size_t)

static block *free_list;
static int ready;

static void init(void) {
  free_list = (block *)__c166_heap;
  free_list->size = __c166_heap_size & ~(size_t)1;
  free_list->next = NULL;
  ready = 1;
}

void *malloc(size_t size) {
  if (!ready)
    init();
  if (size > (size_t)-1 - HEADER - 1)
    return NULL;
  size_t need = (size + HEADER + 1) & ~(size_t)1;
  if (need < sizeof(block))
    need = sizeof(block);
  for (block **pp = &free_list; *pp; pp = &(*pp)->next) {
    block *b = *pp;
    if (b->size < need)
      continue;
    if (b->size - need >= sizeof(block) + 2) {
      block *rest = (block *)((char *)b + need);
      rest->size = b->size - need;
      rest->next = b->next;
      *pp = rest;
      b->size = need;
    } else {
      *pp = b->next;
    }
    return (char *)b + HEADER;
  }
  return NULL;
}

void free(void *p) {
  if (!p)
    return;
  block *b = (block *)((char *)p - HEADER);
  block *prev = NULL, *next = free_list;
  while (next && (char *)next < (char *)b)
    prev = next, next = next->next;
  if (next && (char *)b + b->size == (char *)next) {
    b->size += next->size;
    next = next->next;
  }
  b->next = next;
  if (prev && (char *)prev + prev->size == (char *)b) {
    prev->size += b->size;
    prev->next = next;
  } else if (prev) {
    prev->next = b;
  } else {
    free_list = b;
  }
}

void *calloc(size_t n, size_t size) {
  if (size && n > (size_t)-1 / size)
    return NULL;
  void *p = malloc(n * size);
  if (p)
    memset(p, 0, n * size);
  return p;
}

void *realloc(void *p, size_t size) {
  if (!p)
    return malloc(size);
  if (!size) {
    free(p);
    return NULL;
  }
  size_t old = ((block *)((char *)p - HEADER))->size - HEADER;
  if (size <= old)
    return p;
  void *q = malloc(size);
  if (q) {
    memcpy(q, p, old);
    free(p);
  }
  return q;
}
