#ifndef ULTRIUM_TESTS_INTERNAL_H
#define ULTRIUM_TESTS_INTERNAL_H
#include <stddef.h>
/* A NULL reason means pass. Tests own their cleanup and must leave allocator
   counters at their entry values, including after failure. */
typedef const char *(*test_fn)(void);
struct test_case { const char *name; test_fn run; };

extern const struct test_case storage_cases[];
extern const size_t storage_case_count;
extern const struct test_case ultriumfs_cases[];
extern const size_t ultriumfs_case_count;
extern const struct test_case storage_disk_cases[];
extern const size_t storage_disk_case_count;
#endif
