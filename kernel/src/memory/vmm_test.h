#ifndef ULTRIUM_VMM_TEST_H
#define ULTRIUM_VMM_TEST_H
#include <stdint.h>
/* Fail backing allocation after N successes; pass -1 to disable. */
void vmm_test_fail_alloc_after(int32_t successful_allocations);
#endif
