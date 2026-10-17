#ifndef ULTRIUM_PROCESS_CMD_H
#define ULTRIUM_PROCESS_CMD_H
#include <stddef.h>
#include "ush.h"

/* Shell commands for processes: `run`, `jobs`, `terminate` and `ultrium system processes`.
   `name` is the operation without the "process " prefix: run, jobs, terminate, list. */
enum ush_result process_operation(const char *name, size_t argc, char **argv);
/* Collect finished background processes, printing one line for each; returns how many. */
unsigned process_report_finished(void);
#endif
