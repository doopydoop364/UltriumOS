#ifndef ULTRIUM_VIRTIO_BLK_H
#define ULTRIUM_VIRTIO_BLK_H
#include <stddef.h>

/* Modern (virtio 1.x) virtio-pci block driver. Polled, one request in flight.
   Legacy-only (0.9.5) devices are not supported. */
size_t virtio_blk_probe(void);       /* returns the number of devices registered */
const char *virtio_blk_last_error(void);
#endif
