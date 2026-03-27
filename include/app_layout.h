#ifndef APP_LAYOUT_H
#define APP_LAYOUT_H

// Virtual address where .app images are mapped inside each task address space.
// Must be 2MiB-aligned because tasks map apps via a single 2MiB page.
//
// NOTE: Do not place this in low memory (e.g. 0x800000) because kernel heap
// allocations can grow into that range and would get shadowed by the per-task
// app mapping.
#define MLJOS_APP_VADDR 0x40000000ULL

#endif

