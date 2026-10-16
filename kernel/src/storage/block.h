#ifndef ULTRIUM_BLOCK_H
#define ULTRIUM_BLOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kernel block-device abstraction. A device exposes `block_count` logical
   blocks of `block_size` bytes (a power of two, 512..4096; never assumed to
   be 512). Every transfer is validated here before reaching a driver. The
   registry is single-CPU and unsynchronized, like the other allocators. */
#define BLOCK_MAX_DEVICES 8U
#define BLOCK_NAME_MAX 15U
#define BLOCK_SERIAL_MAX 20U
#define BLOCK_MIN_SIZE 512U
#define BLOCK_MAX_SIZE 4096U

enum block_error {
    BLOCK_OK = 0,
    BLOCK_INVALID,      /* null/misaligned arguments or buffer too small */
    BLOCK_RANGE,        /* request outside the device or arithmetic overflow */
    BLOCK_READ_ONLY,    /* write or flush on a read-only device */
    BLOCK_IO,           /* device reported an error */
    BLOCK_TIMEOUT,      /* bounded wait expired; device is marked dead */
    BLOCK_NO_DEVICE,    /* unknown, removed or dead device */
    BLOCK_UNSUPPORTED   /* operation not provided (e.g. flush) */
};

enum block_flags {
    BLOCK_FLAG_READ_ONLY = 1U,   /* writes are rejected by the core */
    BLOCK_FLAG_FLUSH = 2U,       /* device implements flush */
    BLOCK_FLAG_SCRATCH = 4U,     /* disposable test disk: formatting is permitted */
    BLOCK_FLAG_DEAD = 8U         /* set after a timeout; all I/O refused */
};

struct block_device;
struct block_ops {
    /* One logical block per call; the core loops and validates ranges. */
    enum block_error (*read)(struct block_device *device, uint64_t block, void *buffer);
    enum block_error (*write)(struct block_device *device, uint64_t block, const void *buffer);
    enum block_error (*flush)(struct block_device *device);
};

struct block_device {
    char name[BLOCK_NAME_MAX + 1U];
    char serial[BLOCK_SERIAL_MAX + 1U];
    const char *driver;
    uint32_t block_size;
    uint64_t block_count;
    uint32_t flags;
    const struct block_ops *ops;
    void *context;
    /* Sub-device window: set by block_register_window(). */
    struct block_device *parent;
    uint64_t parent_first;
    /* Statistics and test hooks. */
    uint64_t reads, writes, flushes, errors;
    uint32_t holders;   /* filesystems or windows using the device */
    bool registered;
};

/* Registration copies nothing: the descriptor must outlive the registration.
   block_size must be a power of two within limits; block_count nonzero. */
bool block_register(struct block_device *device);
bool block_unregister(struct block_device *device);
/* A window onto [first, first+count) of an already registered device, for
   partitions or restricted views. It inherits the parent's flags. */
bool block_register_window(struct block_device *window, struct block_device *parent,
                           const char *name, uint64_t first, uint64_t count);

/* A filesystem claims its device while mounted; claimed devices cannot be
   unregistered or destroyed. */
bool block_claim(struct block_device *device);
void block_release(struct block_device *device);

size_t block_count_devices(void);
struct block_device *block_device_at(size_t index);
struct block_device *block_find(const char *name);

enum block_error block_read(struct block_device *device, uint64_t first, uint64_t count,
                            void *buffer, size_t buffer_size);
enum block_error block_write(struct block_device *device, uint64_t first, uint64_t count,
                             const void *buffer, size_t buffer_size);
enum block_error block_flush(struct block_device *device);
const char *block_error_name(enum block_error error);

/* Deterministic failure injection (tests): the next n successful block
   transfers pass, then one transfer fails with BLOCK_IO. Negative disables. */
void block_test_fail_after(int32_t successful_transfers);

/* Memory-backed test device on vmm region pages. count*size <= 16 MiB. */
struct block_device *block_memory_create(const char *name, uint32_t block_size,
                                         uint64_t block_count, bool read_only);
bool block_memory_destroy(struct block_device *device);
#endif
