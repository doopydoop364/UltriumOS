#ifndef ULTRIUM_TESTS_INTERNAL_H
#define ULTRIUM_TESTS_INTERNAL_H
#include <stddef.h>
/* A NULL reason means pass. Tests own their cleanup and must leave allocator
   counters at their entry values, including after failure. */
typedef const char *(*test_fn)(void);
struct test_case { const char *name; test_fn run; };

void tests_serial_text(const char *text);
void tests_serial_number(uint64_t value);
extern const struct test_case storage_cases[];
extern const size_t storage_case_count;
extern const struct test_case ultriumfs_cases[];
extern const size_t ultriumfs_case_count;
extern const struct test_case ufs2_journal_cases[];
extern const size_t ufs2_journal_case_count;
extern const struct test_case ufs2_fs_cases[];
extern const size_t ufs2_fs_case_count;
extern const struct test_case ufs2_crash_cases[];
extern const size_t ufs2_crash_case_count;
unsigned long ufs2_crash_total_runs(void);
extern const struct test_case persist_v2_big_cases[1];
extern const struct test_case persist_v2_bigverify_cases[1];
extern const struct test_case persist_v2_cut_dataflush_cases[1];
extern const struct test_case persist_v2_cut_journal_cases[1];
extern const struct test_case persist_v2_cut_commit_cases[1];
extern const struct test_case persist_v2_cut_home_cases[1];
extern const struct test_case persist_v2_cut_checkpoint_cases[1];
extern const struct test_case persist_v2_recovered_cases[1];
extern const struct test_case persist_v2_stream_cases[1];
extern const struct test_case persist_v2_streamverify_cases[1];
extern const struct test_case task_cases[];
extern const size_t task_case_count;
extern const struct test_case storage_disk_cases[];
extern const size_t storage_disk_case_count;
extern const struct test_case persist_blank_cases[], persist_populate_cases[], persist_stage1_cases[],
    persist_modify_cases[], persist_stage3_cases[], persist_clean_cases[], persist_unclean_cases[], persist_v1fixture_cases[], persist_v1verify_cases[];
extern const size_t persist_blank_count, persist_populate_count, persist_stage1_count, persist_modify_count,
    persist_stage3_count, persist_clean_count, persist_unclean_count, persist_v1fixture_count, persist_v1verify_count;
#endif
