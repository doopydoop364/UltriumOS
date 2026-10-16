#ifndef ULTRIUM_USH_H
#define ULTRIUM_USH_H
#include <stdbool.h>
#include <stddef.h>
#include "fs/vfs.h"
#define USH_VERSION "0.6"
#define USH_INPUT_MAX 70U
#define USH_ARG_MAX 16U
enum ush_result { USH_OK, USH_UNKNOWN, USH_USAGE, USH_UNAVAILABLE, USH_FS_ERROR, USH_PENDING };
struct ush_args {
    size_t count;
    bool trailing_space;
    char *values[USH_ARG_MAX];
    size_t starts[USH_ARG_MAX];
    const char *error;
    char storage[USH_INPUT_MAX + 1U];
};
enum ush_completion { USH_COMPLETE_NONE, USH_COMPLETE_UNIQUE, USH_COMPLETE_MULTIPLE, USH_COMPLETE_LIMIT };
enum ush_result ush_tokenize_prefix(const char *line, struct ush_args *args);
enum ush_completion ush_complete(char line[USH_INPUT_MAX + 1U], size_t *caret);
void ush_completion_show(void);
const char *ush_suggest(const char *parent, const char *word);
bool ush_word_equal(const char *a, const char *b);
enum ush_result ush_tokenize(const char *line, struct ush_args *args);
enum ush_result ush_execute(const char *line);
enum ush_result ush_operation(const char *name, size_t argc, char **argv);
bool ush_registry_valid(void);
bool ush_request_confirmation(const char *question, void (*answer)(bool));
bool ush_confirmation_pending(void);
const char *ush_confirmation_prompt(void);
enum ush_result ush_answer_confirmation(const char *line);
const char *ush_cwd(void);
enum ush_result ush_fs_failure(enum vfs_error error);
bool ush_compatibility(void);
void ush_version(void);
void ush_compatibility_status(void);
void ush_compatibility_enable(void);
void ush_compatibility_disable(void);
#endif
