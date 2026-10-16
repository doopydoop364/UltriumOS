#ifndef ULTRIUM_STORAGE_OPS_H
#define ULTRIUM_STORAGE_OPS_H
#include <stdbool.h>
#include <stddef.h>
#include "ush.h"

/* `ultrium storage format|mount|unmount|sync|volumes|info`. `name` is the
   operation after the "storage " prefix. */
enum ush_result storage_operation(const char *name, size_t argc, char **argv);
/* Shutdown sequence pieces, also used by tests: freeze, sync and flush every
   persistent volume; false means data may not be persisted. */
bool storage_shutdown_sync(size_t *volumes, size_t *synced);
void storage_shutdown_abort(void);
/* Registers the shutdown hook (freeze, sync, flush) with the power module. */
void storage_register_power_hook(void);
#endif
