#ifndef ULTRIUM_POWER_H
#define ULTRIUM_POWER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>

/* ACPI soft-off discovery and request. Tables are located from the Limine
   RSDP and read through the higher-half direct map only after the physical
   range is confirmed to lie inside a Limine memory-map entry. */
void power_init(const struct limine_memmap_response *memmap, uint64_t hhdm, uint64_t rsdp_virtual);
bool power_available(void);
void power_status(void);
/* Disables interrupts, requests ACPI S5 and does not return on success. On
   failure it prints the reason, re-enables interrupts if they were enabled
   and returns so the shell stays usable. */
void power_shutdown(void);

/* Filesystem synchronization before power-off. `prepare` stops accepting
   changes, syncs every persistent filesystem and flushes devices; `abort`
   resumes normal operation after a failed shutdown. */
struct power_sync_report { size_t volumes, synced; bool ok; };
void power_set_sync_hooks(void (*prepare)(struct power_sync_report *), void (*abort)(void));
#endif
