#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "storage/block.h"
#include "memory/pmm.h"
#include "memory.h"

/* Block-layer tests run on memory-backed devices so they work on diskless
   boots. The disk cases at the bottom touch only a scratch-flagged virtio
   disk and only blocks in a reserved window at the end of the device. */

static uint8_t buf_a[4096 * 4], buf_b[4096 * 4];

static void pattern(uint8_t *out, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(seed + i * 7U + (i >> 8));
}
#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)

static const char *block_registry(void) {
    uint64_t frames = pmm_free_pages();
    const char *result = 0;
    struct block_device *d = block_memory_create("memtest", 512, 64, false);
    REQUIRE(d, "memory device creation failed");
    if (block_find("memtest") != d) result = "device not found by name";
    else if (block_memory_create("memtest", 512, 64, false)) result = "duplicate name accepted";
    else if (block_memory_create("Bad Name", 512, 64, false)) result = "invalid name accepted";
    else if (block_memory_create("odd", 600, 64, false)) result = "non-power-of-two block size accepted";
    else if (block_memory_create("zero", 512, 0, false)) result = "zero-capacity device accepted";
    else if (block_find("missing") || block_find(0)) result = "lookup of unknown device succeeded";
    else if (d->block_size != 512 || d->block_count != 64) result = "geometry not reported";
    if (!block_memory_destroy(d) && !result) result = "destroy failed";
    if (!result && block_find("memtest")) result = "device remains after destroy";
    if (!result && pmm_free_pages() != frames) result = "frame accounting changed";
    return result;
}

static const char *block_round_trip(void) {
    uint64_t frames = pmm_free_pages();
    const char *result = 0;
    static const uint32_t sizes[] = {512, 1024, 4096};
    for (unsigned s = 0; s < 3 && !result; ++s) {
        uint32_t size = sizes[s];
        struct block_device *d = block_memory_create("rt", size, 32, false);
        if (!d) return "memory device creation failed";
        /* Distinct blocks, then read back individually and as a span. */
        for (uint64_t b = 0; b < 32 && !result; ++b) {
            pattern(buf_a, size, (uint8_t)(b * 13 + s));
            if (block_write(d, b, 1, buf_a, size) != BLOCK_OK) result = "block write failed";
        }
        for (uint64_t b = 0; b < 32 && !result; ++b) {
            pattern(buf_a, size, (uint8_t)(b * 13 + s));
            memset(buf_b, 0, size);
            if (block_read(d, b, 1, buf_b, size) != BLOCK_OK) result = "block read failed";
            else if (memcmp(buf_a, buf_b, size)) result = "block data mismatch (isolation)";
        }
        if (!result) {
            size_t span = size * 4U > sizeof(buf_b) ? sizeof(buf_b) : size * 4U;
            if (block_read(d, 8, 4, buf_b, span) != BLOCK_OK) result = "multi-block read failed";
            for (unsigned k = 0; k < 4 && !result; ++k) {
                pattern(buf_a, size, (uint8_t)((8 + k) * 13 + s));
                if (memcmp(buf_a, buf_b + k * size, size)) result = "multi-block data mismatch";
            }
        }
        /* Repeated overwrite of one block. */
        for (unsigned r = 0; r < 100 && !result; ++r) {
            pattern(buf_a, size, (uint8_t)(r + 1));
            if (block_write(d, 5, 1, buf_a, size) != BLOCK_OK ||
                block_read(d, 5, 1, buf_b, size) != BLOCK_OK || memcmp(buf_a, buf_b, size))
                result = "repeated overwrite mismatch";
        }
        if (!block_memory_destroy(d) && !result) result = "destroy failed";
    }
    if (!result && pmm_free_pages() != frames) result = "frame accounting changed";
    return result;
}

static const char *block_bounds(void) {
    uint64_t frames = pmm_free_pages();
    struct block_device *d = block_memory_create("bounds", 512, 16, false);
    REQUIRE(d, "memory device creation failed");
    const char *result = 0;
    memset(buf_a, 0xa5, 512);
    if (block_read(d, 16, 1, buf_b, 512) != BLOCK_RANGE) result = "read past end accepted";
    else if (block_write(d, 16, 1, buf_a, 512) != BLOCK_RANGE) result = "write past end accepted";
    else if (block_read(d, 15, 2, buf_b, 1024) != BLOCK_RANGE) result = "span crossing end accepted";
    else if (block_read(d, UINT64_MAX, 1, buf_b, 512) != BLOCK_RANGE) result = "huge first block accepted";
    else if (block_read(d, 0, UINT64_MAX, buf_b, 512) != BLOCK_RANGE) result = "count overflow accepted";
    else if (block_read(d, 1, UINT64_MAX, buf_b, 512) != BLOCK_RANGE) result = "first+count overflow accepted";
    else if (block_read(d, 0, 0, buf_b, 512) != BLOCK_INVALID) result = "zero-length transfer accepted";
    else if (block_read(d, 0, 1, 0, 512) != BLOCK_INVALID) result = "null buffer accepted";
    else if (block_read(d, 0, 2, buf_b, 512) != BLOCK_INVALID) result = "short buffer accepted";
    else if (block_write(d, 0, 1, buf_a, 511) != BLOCK_INVALID) result = "short write buffer accepted";
    else if (block_read(0, 0, 1, buf_b, 512) != BLOCK_NO_DEVICE) result = "null device accepted";
    else if (block_write(d, 15, 1, buf_a, 512) != BLOCK_OK) result = "last block write rejected";
    else if (block_read(d, 15, 1, buf_b, 512) != BLOCK_OK || memcmp(buf_a, buf_b, 512)) result = "last block read-back failed";
    if (!block_memory_destroy(d) && !result) result = "destroy failed";
    if (!result && pmm_free_pages() != frames) result = "frame accounting changed";
    return result;
}

static const char *block_read_only(void) {
    uint64_t frames = pmm_free_pages();
    struct block_device *d = block_memory_create("rodev", 512, 8, true);
    REQUIRE(d, "read-only memory device creation failed");
    const char *result = 0;
    memset(buf_a, 0xcc, 512);
    if (block_write(d, 0, 1, buf_a, 512) != BLOCK_READ_ONLY) result = "write to read-only device accepted";
    else if (block_flush(d) != BLOCK_READ_ONLY) result = "flush of read-only device accepted";
    else if (block_read(d, 0, 1, buf_b, 512) != BLOCK_OK) result = "read from read-only device failed";
    else {
        for (unsigned i = 0; i < 512; ++i) if (buf_b[i]) { result = "read-only device was modified"; break; }
    }
    if (!block_memory_destroy(d) && !result) result = "destroy failed";
    if (!result && pmm_free_pages() != frames) result = "frame accounting changed";
    return result;
}

static const char *block_failures(void) {
    uint64_t frames = pmm_free_pages();
    struct block_device *d = block_memory_create("faulty", 512, 8, false);
    REQUIRE(d, "memory device creation failed");
    const char *result = 0;
    pattern(buf_a, 512, 3);
    block_test_fail_after(1);
    if (block_write(d, 0, 1, buf_a, 512) != BLOCK_OK) result = "first transfer should pass";
    else if (block_write(d, 1, 1, buf_a, 512) != BLOCK_IO) result = "injected write failure not reported";
    else if (d->errors != 1) result = "error counter not updated";
    else if (block_write(d, 1, 1, buf_a, 512) != BLOCK_OK) result = "device did not recover after one failure";
    block_test_fail_after(0);
    if (!result && block_read(d, 0, 1, buf_b, 512) != BLOCK_IO) result = "injected read failure not reported";
    block_test_fail_after(-1);
    if (!result && (block_read(d, 0, 1, buf_b, 512) != BLOCK_OK || memcmp(buf_a, buf_b, 512)))
        result = "data damaged by failed transfers";
    /* Multi-block write that fails midway: the call reports the error. */
    memset(buf_a, 0x11, 1024);
    block_test_fail_after(1);
    if (!result && block_write(d, 2, 2, buf_a, 1024) != BLOCK_IO) result = "mid-span failure not reported";
    block_test_fail_after(-1);
    /* A dead device refuses I/O. */
    d->flags |= BLOCK_FLAG_DEAD;
    if (!result && (block_read(d, 0, 1, buf_b, 512) != BLOCK_NO_DEVICE || block_flush(d) != BLOCK_NO_DEVICE))
        result = "dead device accepted I/O";
    d->flags &= ~BLOCK_FLAG_DEAD;
    if (!block_memory_destroy(d) && !result) result = "destroy failed";
    if (!result && pmm_free_pages() != frames) result = "frame accounting changed";
    return result;
}

static const char *block_windows(void) {
    uint64_t frames = pmm_free_pages();
    struct block_device *parent = block_memory_create("whole", 512, 32, false);
    REQUIRE(parent, "memory device creation failed");
    static struct block_device window, second;
    const char *result = 0;
    if (block_register_window(&second, parent, "bad", 30, 3)) result = "window past end accepted";
    else if (block_register_window(&second, parent, "bad", 33, 1)) result = "window starting past end accepted";
    else if (block_register_window(&second, parent, "bad", 0, 0)) result = "empty window accepted";
    else if (!block_register_window(&window, parent, "part", 8, 8)) result = "window creation failed";
    else if (window.block_count != 8 || window.block_size != 512) result = "window geometry wrong";
    else {
        pattern(buf_a, 512, 9);
        if (block_write(&window, 0, 1, buf_a, 512) != BLOCK_OK) result = "window write failed";
        else if (block_read(parent, 8, 1, buf_b, 512) != BLOCK_OK || memcmp(buf_a, buf_b, 512)) result = "window offset not applied";
        else if (block_write(&window, 8, 1, buf_a, 512) != BLOCK_RANGE) result = "window escaped its bounds";
        else if (block_memory_destroy(parent)) result = "parent destroyed while window exists";
        else if (!block_unregister(&window)) result = "window unregister failed";
    }
    if (!block_memory_destroy(parent) && !result) result = "destroy failed";
    if (!result && pmm_free_pages() != frames) result = "frame accounting changed";
    return result;
}

static uint8_t tiny_storage[4][512];
static enum block_error tiny_read(struct block_device *d, uint64_t b, void *out) { (void)d; memcpy(out, tiny_storage[b], 512); return BLOCK_OK; }
static enum block_error tiny_write(struct block_device *d, uint64_t b, const void *in) { (void)d; memcpy(tiny_storage[b], in, 512); return BLOCK_OK; }
static const struct block_ops tiny_ops = {tiny_read, tiny_write, 0};

static const char *block_capabilities(void) {
    static struct block_device tiny = {.name = "tiny", .driver = "test", .block_size = 512, .block_count = 4, .ops = &tiny_ops};
    const char *result = 0;
    if (!block_register(&tiny)) return "registration of device without flush failed";
    if (block_flush(&tiny) != BLOCK_UNSUPPORTED) result = "flush on non-flush device should be unsupported";
    else if (block_register(&tiny)) result = "double registration accepted";
    if (!block_unregister(&tiny) && !result) result = "unregister failed";
    if (!result && (block_read(&tiny, 0, 1, buf_a, 512) != BLOCK_NO_DEVICE))
        result = "I/O on removed device accepted";
    /* A writable device without a write operation must be rejected. */
    static const struct block_ops no_write = {tiny_read, 0, 0};
    static struct block_device broken = {.name = "broken", .driver = "test", .block_size = 512, .block_count = 1, .ops = &no_write};
    if (!result && block_register(&broken)) { block_unregister(&broken); result = "writable device without write op accepted"; }
    return result;
}

const struct test_case storage_cases[] = {
    {"block device registry", block_registry},
    {"block round trip and isolation", block_round_trip},
    {"block bounds and overflow", block_bounds},
    {"block read-only enforcement", block_read_only},
    {"block failure injection", block_failures},
    {"block windows", block_windows},
    {"block capabilities and removal", block_capabilities}
};
const size_t storage_case_count = sizeof(storage_cases) / sizeof(storage_cases[0]);

/* ---- scratch virtio disk (hardware path) ---- */
#define RESERVED_BLOCKS 16U

static struct block_device *scratch_disk(void) {
    for (size_t i = 0; i < block_count_devices(); ++i) {
        struct block_device *d = block_device_at(i);
        if (d->driver && d->driver[0] == 'v' && (d->flags & BLOCK_FLAG_SCRATCH)) return d;
    }
    return 0;
}

static const char *disk_identity(void) {
    struct block_device *d = scratch_disk();
    REQUIRE(d, "no scratch virtio disk attached");
    REQUIRE(d->block_size >= 512 && d->block_size <= 4096 && !(d->block_size & (d->block_size - 1)), "invalid block size");
    REQUIRE(d->block_count * d->block_size >= (1ULL << 20), "capacity below 1 MiB");
    REQUIRE(d->block_count > RESERVED_BLOCKS * 2, "disk too small for tests");
    return 0;
}

static const char *disk_read_write(void) {
    struct block_device *d = scratch_disk();
    REQUIRE(d, "no scratch virtio disk attached");
    uint32_t size = d->block_size;
    uint64_t base = d->block_count - RESERVED_BLOCKS;
    if (d->flags & BLOCK_FLAG_READ_ONLY) {
        pattern(buf_a, size, 1);
        REQUIRE(block_write(d, base, 1, buf_a, size) == BLOCK_READ_ONLY, "write to read-only disk accepted");
        REQUIRE(block_flush(d) == BLOCK_READ_ONLY, "flush of read-only disk accepted");
        REQUIRE(block_read(d, base, 1, buf_b, size) == BLOCK_OK, "read from read-only disk failed");
        return 0;
    }
    for (uint64_t b = 0; b < RESERVED_BLOCKS; ++b) {
        pattern(buf_a, size, (uint8_t)(b * 5 + 1));
        REQUIRE(block_write(d, base + b, 1, buf_a, size) == BLOCK_OK, "disk write failed");
    }
    if (d->flags & BLOCK_FLAG_FLUSH) REQUIRE(block_flush(d) == BLOCK_OK, "disk flush failed");
    for (uint64_t b = 0; b < RESERVED_BLOCKS; ++b) {
        pattern(buf_a, size, (uint8_t)(b * 5 + 1));
        memset(buf_b, 0, size);
        REQUIRE(block_read(d, base + b, 1, buf_b, size) == BLOCK_OK, "disk read failed");
        REQUIRE(!memcmp(buf_a, buf_b, size), "disk data mismatch");
    }
    for (unsigned r = 0; r < 100; ++r) {
        pattern(buf_a, size, (uint8_t)(r + 40));
        REQUIRE(block_write(d, base + (r % RESERVED_BLOCKS), 1, buf_a, size) == BLOCK_OK &&
                block_read(d, base + (r % RESERVED_BLOCKS), 1, buf_b, size) == BLOCK_OK &&
                !memcmp(buf_a, buf_b, size), "repeated disk transfer mismatch");
    }
    return 0;
}

static const char *disk_bounds(void) {
    struct block_device *d = scratch_disk();
    REQUIRE(d, "no scratch virtio disk attached");
    uint32_t size = d->block_size;
    REQUIRE(block_read(d, d->block_count, 1, buf_b, size) == BLOCK_RANGE, "read past end of disk accepted");
    REQUIRE(block_read(d, d->block_count - 1, 2, buf_b, size * 2U) == BLOCK_RANGE, "span past end of disk accepted");
    REQUIRE(block_read(d, d->block_count - 1, 1, buf_b, size) == BLOCK_OK, "last block unreadable");
    if (!(d->flags & BLOCK_FLAG_READ_ONLY))
        REQUIRE(block_write(d, d->block_count, 1, buf_a, size) == BLOCK_RANGE, "write past end of disk accepted");
    return 0;
}

static const char *disk_multi_block(void) {
    struct block_device *d = scratch_disk();
    REQUIRE(d, "no scratch virtio disk attached");
    if (d->flags & BLOCK_FLAG_READ_ONLY) return 0;
    uint32_t size = d->block_size;
    uint64_t base = d->block_count - RESERVED_BLOCKS;
    size_t span = (size_t)size * 3U;
    pattern(buf_a, span, 77);
    REQUIRE(block_write(d, base, 3, buf_a, span) == BLOCK_OK, "multi-block disk write failed");
    memset(buf_b, 0, span);
    REQUIRE(block_read(d, base, 3, buf_b, span) == BLOCK_OK, "multi-block disk read failed");
    REQUIRE(!memcmp(buf_a, buf_b, span), "multi-block disk mismatch");
    return 0;
}

const struct test_case storage_disk_cases[] = {
    {"virtio disk identity and capacity", disk_identity},
    {"virtio disk read/write and flush", disk_read_write},
    {"virtio disk bounds", disk_bounds},
    {"virtio disk multi-block", disk_multi_block}
};
const size_t storage_disk_case_count = sizeof(storage_disk_cases) / sizeof(storage_disk_cases[0]);
