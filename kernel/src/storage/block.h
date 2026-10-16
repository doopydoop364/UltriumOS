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

/* Volatile-cache crash model (tests only). With the cache enabled, writes stay
   pending until a flush; a simulated power cut keeps all, none, a prefix, a
   seeded-random subset, or a torn prefix of the pending writes. */
enum block_power_policy { POWER_KEEP_ALL, POWER_LOSE_ALL, POWER_KEEP_PREFIX, POWER_RANDOM, POWER_TORN,
                         POWER_KEEP_SUFFIX /* keep only the last `parameter` pending writes: models reordering */ };
bool block_memory_enable_cache(struct block_device *device);
/* Cut power before executing I/O event `events` (writes and flushes counted
   from now, 0 = before the first). All I/O then fails until a power cycle. */
void block_memory_crash_after(struct block_device *device, int64_t events);
void block_memory_power_cycle(struct block_device *device, enum block_power_policy policy, uint32_t parameter);
uint64_t block_memory_events(struct block_device *device);
bool block_memory_crashed(struct block_device *device);
const char *block_memory_crash_phase(struct block_device *device);
uint32_t block_memory_pending(struct block_device *device);
bool block_memory_copy(struct block_device *to, struct block_device *from);
/* Fast reset for matrix tests: take a snapshot once, then restore only the blocks written since. */
bool block_memory_snapshot(struct block_device *device);
void block_memory_restore(struct block_device *device);

/* Deterministic power cut for runs on real devices (QEMU): when the `occurrence`-th
   block_set_phase(phase) call happens, every later transfer or flush on every device
   fails with BLOCK_IO and `hook` runs once. The cut is global until block_restore_power(). */
void block_arm_power_cut(const char *phase, int occurrence, void (*hook)(const char *phase));
void block_restore_power(void);
bool block_power_is_cut(void);
/* Label of the I/O phase in progress; recorded when a crash point fires. */
void block_set_phase(const char *phase);
const char *block_get_phase(void);
#endif
