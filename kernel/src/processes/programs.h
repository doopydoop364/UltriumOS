#ifndef ULTRIUM_PROGRAMS_H
#define ULTRIUM_PROGRAMS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native programs built from os/programs and embedded in the kernel image. They are only a delivery
   mechanism: programs_install() copies them into the root filesystem (/bin/<name>) and everything
   after that, including `run`, loads them from the VFS like any other file. */
#define PROGRAMS_DIRECTORY "/bin"
unsigned programs_count(void);
const char *programs_name(unsigned index);
bool programs_image(unsigned index, const uint8_t **data, size_t *size);
bool programs_install(void);           /* idempotent; returns false if any program could not be written */
#endif
