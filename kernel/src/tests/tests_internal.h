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
extern const struct test_case ufs2_journal_cases[];
extern const size_t ufs2_journal_case_count;
extern const struct test_case storage_disk_cases[];
extern const size_t storage_disk_case_count;
extern const struct test_case persist_blank_cases[], persist_populate_cases[], persist_stage1_cases[],
    persist_modify_cases[], persist_stage3_cases[], persist_clean_cases[], persist_unclean_cases[];
extern const size_t persist_blank_count, persist_populate_count, persist_stage1_count, persist_modify_count,
    persist_stage3_count, persist_clean_count, persist_unclean_count;
#endif
