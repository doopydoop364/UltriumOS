#ifndef ULTRIUM_HEAP_H
#define ULTRIUM_HEAP_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
bool heap_init(void);
void *kmalloc(size_t bytes);
void kfree(void *pointer);
uint64_t heap_live_allocations(void);
bool heap_self_test(void);
#endif
