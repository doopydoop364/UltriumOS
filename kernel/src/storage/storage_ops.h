#ifndef ULTRIUM_STORAGE_OPS_H
#define ULTRIUM_STORAGE_OPS_H
#include <stdbool.h>
#include <stddef.h>
#include "ush.h"
#include "block.h"

/* `ultrium storage format|mount|unmount|sync|volumes|info`. `name` is the
   operation after the "storage " prefix. */
enum ush_result storage_operation(const char *name, size_t argc, char **argv);
/* Which UltriumFS format a device holds (probe reads only). */
enum storage_format { STORAGE_UNKNOWN, STORAGE_V1, STORAGE_V2 };
enum storage_format storage_probe(struct block_device *device);
struct storage_volume { int version; bool read_only, faulted, unclean, repair_recommended; uint32_t orphans; };
/* Status of the mounted UltriumFS volume, whichever version. */
bool storage_volume_status(struct storage_volume *volume);

/* Shutdown sequence pieces, also used by tests: freeze, sync and flush every
   persistent volume; false means data may not be persisted. */
bool storage_shutdown_sync(size_t *volumes, size_t *synced);
void storage_shutdown_abort(void);
/* Registers the shutdown hook (freeze, sync, flush) with the power module. */
void storage_register_power_hook(void);
#endif
