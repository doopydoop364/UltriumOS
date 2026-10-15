#ifndef ULTRIUM_USH_H
#define ULTRIUM_USH_H
#include <stdbool.h>
#include <stddef.h>
#include "fs/vfs.h"
#define USH_VERSION "0.2"
#define USH_INPUT_MAX 70U
#define USH_ARG_MAX 16U
enum ush_result { USH_OK, USH_UNKNOWN, USH_USAGE, USH_UNAVAILABLE, USH_FS_ERROR };
struct ush_args { size_t count; char *values[USH_ARG_MAX]; char storage[USH_INPUT_MAX + 1U]; };
bool ush_word_equal(const char *a, const char *b);
enum ush_result ush_tokenize(const char *line, struct ush_args *args);
enum ush_result ush_execute(const char *line);
enum ush_result ush_operation(const char *name, size_t argc, char **argv);
bool ush_registry_valid(void);
const char *ush_cwd(void);
bool ush_compatibility(void);
void ush_help(void);
void ush_version(void);
void ush_compatibility_help(void);
void ush_compatibility_status(void);
void ush_compatibility_enable(void);
void ush_compatibility_disable(void);
#endif
