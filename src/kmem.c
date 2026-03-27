#include "kmem.h"

extern char _kernel_end;

static uintptr_t g_heap = 0;

static uintptr_t align_up(uintptr_t v, uint64_t align) {
    if (align == 0) return v;
    uintptr_t mask = (uintptr_t)(align - 1);
    return (v + mask) & ~mask;
}

void kmem_init(void) {
    // Start heap after kernel image. Keep a small gap for safety.
    uintptr_t start = (uintptr_t)&_kernel_end;
    g_heap = align_up(start + 0x10000, 16);
}

void *kmem_alloc(uint64_t size, uint64_t align) {
    if (size == 0) size = 1;
    if (align < 16) align = 16;
    uintptr_t p = align_up(g_heap, align);
    g_heap = p + (uintptr_t)size;
    return (void *)p;
}

void kmem_memset(void *dst, uint8_t value, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < size; ++i) d[i] = value;
}

void kmem_memcpy(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < size; ++i) d[i] = s[i];
}

