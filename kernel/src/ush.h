#ifndef ULTRIUM_USH_H
#define ULTRIUM_USH_H
#include <stdbool.h>
#include <stddef.h>
#include "fs/vfs.h"
#define USH_VERSION "0.3"
#define USH_INPUT_MAX 70U
#define USH_ARG_MAX 16U
enum ush_result { USH_OK, USH_UNKNOWN, USH_USAGE, USH_UNAVAILABLE, USH_FS_ERROR, USH_PENDING };
struct ush_args { size_t count; char *values[USH_ARG_MAX]; char storage[USH_INPUT_MAX + 1U]; };
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
bool ush_compatibility(void);
void ush_version(void);
void ush_compatibility_status(void);
void ush_compatibility_enable(void);
void ush_compatibility_disable(void);
#endif
