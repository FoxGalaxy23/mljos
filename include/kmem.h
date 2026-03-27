#ifndef KMEM_H
#define KMEM_H

#include "common.h"

void kmem_init(void);
void *kmem_alloc(uint64_t size, uint64_t align);
void kmem_memset(void *dst, uint8_t value, uint64_t size);
void kmem_memcpy(void *dst, const void *src, uint64_t size);

#endif

