#include <stddef.h>
#include <stdint.h>
#include "block.h"
#include "memory/vmm.h"
#include "memory/heap.h"

static struct block_device *registry[BLOCK_MAX_DEVICES];
static int32_t injected_after = -1;

void block_test_fail_after(int32_t n) { injected_after = n; }

static size_t text_length(const char *text, size_t maximum) {
    size_t n = 0;
    while (n <= maximum && text[n]) ++n;
    return n;
}
static bool name_ok(const char *name) {
    if (!name) return false;
    size_t n = text_length(name, BLOCK_NAME_MAX);
    if (!n || n > BLOCK_NAME_MAX) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}
static void copy_name(char *out, const char *name) {
    size_t i = 0;
    for (; name[i] && i < BLOCK_NAME_MAX; ++i) out[i] = name[i];
    out[i] = 0;
}
static bool size_ok(uint32_t size) {
    return size >= BLOCK_MIN_SIZE && size <= BLOCK_MAX_SIZE && !(size & (size - 1));
}

struct block_device *block_find(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; ++i) {
        if (!registry[i]) continue;
        const char *a = registry[i]->name, *b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) return registry[i];
    }
    return 0;
}
size_t block_count_devices(void) {
    size_t n = 0;
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; ++i) if (registry[i]) ++n;
    return n;
}
struct block_device *block_device_at(size_t index) {
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; ++i)
        if (registry[i] && index-- == 0) return registry[i];
    return 0;
}

bool block_register(struct block_device *d) {
    if (!d || d->registered || !name_ok(d->name) || !d->ops || !d->ops->read ||
        !size_ok(d->block_size) || !d->block_count || d->block_count > (~0ULL / BLOCK_MAX_SIZE) ||
        block_find(d->name)) return false;
    if (!(d->flags & BLOCK_FLAG_READ_ONLY) && !d->ops->write) return false;
    if ((d->flags & BLOCK_FLAG_FLUSH) && !d->ops->flush) return false;
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; ++i) {
        if (!registry[i]) { registry[i] = d; d->registered = true; return true; }
    }
    return false;
}

static bool in_registry(const struct block_device *d) {
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; ++i) if (registry[i] == d) return true;
    return false;
}
bool block_claim(struct block_device *d) {
    if (!d || !d->registered || !in_registry(d) || (d->flags & BLOCK_FLAG_DEAD) || d->holders == UINT32_MAX) return false;
    ++d->holders;
    return true;
}
void block_release(struct block_device *d) { if (d && d->holders) --d->holders; }

bool block_unregister(struct block_device *d) {
    if (!d || !d->registered || d->holders) return false;
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; ++i) {
        if (registry[i] && registry[i]->parent == d) return false;  /* windows still reference it */
    }
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; ++i)
        if (registry[i] == d) { registry[i] = 0; d->registered = false; return true; }
    return false;
}

static enum block_error window_read(struct block_device *w, uint64_t block, void *buffer) {
    return w->parent->ops->read(w->parent, w->parent_first + block, buffer);
}
static enum block_error window_write(struct block_device *w, uint64_t block, const void *buffer) {
    return w->parent->ops->write(w->parent, w->parent_first + block, buffer);
}
static enum block_error window_flush(struct block_device *w) { return w->parent->ops->flush(w->parent); }
static const struct block_ops window_ops = {window_read, window_write, window_flush};

bool block_register_window(struct block_device *w, struct block_device *parent, const char *name,
                           uint64_t first, uint64_t count) {
    if (!w || !parent || !in_registry(parent) || !count || first > parent->block_count ||
        count > parent->block_count - first || !name_ok(name)) return false;
    *w = (struct block_device){0};
    copy_name(w->name, name);
    w->driver = "window";
    w->block_size = parent->block_size; w->block_count = count;
    w->flags = parent->flags & ~BLOCK_FLAG_DEAD;
    if (!(w->flags & BLOCK_FLAG_FLUSH)) w->flags &= ~BLOCK_FLAG_FLUSH;
    w->ops = &window_ops; w->parent = parent; w->parent_first = first;
    if (!block_register(w)) { *w = (struct block_device){0}; return false; }
    return true;
}

static enum block_error validate(struct block_device *d, uint64_t first, uint64_t count,
                                 const void *buffer, size_t buffer_size, bool write) {
    if (!d || !d->registered || !in_registry(d)) return BLOCK_NO_DEVICE;
    if (d->parent && (d->parent->flags & BLOCK_FLAG_DEAD)) return BLOCK_NO_DEVICE;
    if (d->flags & BLOCK_FLAG_DEAD) return BLOCK_NO_DEVICE;
    if (!buffer || !count) return BLOCK_INVALID;
    if (write && (d->flags & BLOCK_FLAG_READ_ONLY)) return BLOCK_READ_ONLY;
    if (first > d->block_count || count > d->block_count - first) return BLOCK_RANGE;
    if (count > (uint64_t)SIZE_MAX / d->block_size) return BLOCK_RANGE;
    if ((uint64_t)buffer_size < count * d->block_size) return BLOCK_INVALID;
    return BLOCK_OK;
}
static bool injected_failure(void) {
    if (injected_after < 0) return false;
    if (injected_after > 0) { --injected_after; return false; }
    injected_after = -1;
    return true;
}

enum block_error block_read(struct block_device *d, uint64_t first, uint64_t count,
                            void *buffer, size_t buffer_size) {
    enum block_error error = validate(d, first, count, buffer, buffer_size, false);
    if (error != BLOCK_OK) return error;
    uint8_t *out = buffer;
    for (uint64_t i = 0; i < count; ++i) {
        error = injected_failure() ? BLOCK_IO : d->ops->read(d, first + i, out + i * d->block_size);
        if (error != BLOCK_OK) { ++d->errors; return error; }
        ++d->reads;
    }
    return BLOCK_OK;
}
enum block_error block_write(struct block_device *d, uint64_t first, uint64_t count,
                             const void *buffer, size_t buffer_size) {
    enum block_error error = validate(d, first, count, buffer, buffer_size, true);
    if (error != BLOCK_OK) return error;
    const uint8_t *in = buffer;
    for (uint64_t i = 0; i < count; ++i) {
        error = injected_failure() ? BLOCK_IO : d->ops->write(d, first + i, in + i * d->block_size);
        if (error != BLOCK_OK) { ++d->errors; return error; }
        ++d->writes;
    }
    return BLOCK_OK;
}
enum block_error block_flush(struct block_device *d) {
    if (!d || !d->registered || !in_registry(d) || (d->flags & BLOCK_FLAG_DEAD) ||
        (d->parent && (d->parent->flags & BLOCK_FLAG_DEAD))) return BLOCK_NO_DEVICE;
    if (d->flags & BLOCK_FLAG_READ_ONLY) return BLOCK_READ_ONLY;
    if (!(d->flags & BLOCK_FLAG_FLUSH)) return BLOCK_UNSUPPORTED;
    enum block_error error = d->ops->flush(d);
    if (error != BLOCK_OK) { ++d->errors; return error; }
    ++d->flushes;
    return BLOCK_OK;
}
const char *block_error_name(enum block_error e) {
    switch (e) {
        case BLOCK_OK: return "ok";
        case BLOCK_INVALID: return "invalid argument";
        case BLOCK_RANGE: return "out of range";
        case BLOCK_READ_ONLY: return "read-only device";
        case BLOCK_IO: return "device I/O error";
        case BLOCK_TIMEOUT: return "device timeout";
        case BLOCK_NO_DEVICE: return "no such device";
        case BLOCK_UNSUPPORTED: return "unsupported operation";
    }
    return "unknown error";
}

/* ---- memory-backed test device ---- */
struct memory_block {
    struct block_device device;
    uint8_t *data;
    uint64_t bytes;
};
static struct memory_block memory_devices[2];

static enum block_error memory_read(struct block_device *d, uint64_t block, void *buffer) {
    struct memory_block *m = d->context;
    uint8_t *source = m->data + block * d->block_size;
    for (uint32_t i = 0; i < d->block_size; ++i) ((uint8_t *)buffer)[i] = source[i];
    return BLOCK_OK;
}
static enum block_error memory_write(struct block_device *d, uint64_t block, const void *buffer) {
    struct memory_block *m = d->context;
    uint8_t *target = m->data + block * d->block_size;
    for (uint32_t i = 0; i < d->block_size; ++i) target[i] = ((const uint8_t *)buffer)[i];
    return BLOCK_OK;
}
static enum block_error memory_flush(struct block_device *d) { (void)d; return BLOCK_OK; }
static const struct block_ops memory_ops = {memory_read, memory_write, memory_flush};

struct block_device *block_memory_create(const char *name, uint32_t block_size,
                                         uint64_t block_count, bool read_only) {
    if (!size_ok(block_size) || !block_count || block_count > (16ULL << 20) / block_size) return 0;
    struct memory_block *m = 0;
    for (size_t i = 0; i < 2; ++i) if (!memory_devices[i].device.registered && !memory_devices[i].data) { m = &memory_devices[i]; break; }
    if (!m) return 0;
    uint64_t bytes = block_count * block_size;
    m->data = vmm_alloc_pages((size_t)((bytes + 4095) / 4096));
    if (!m->data) return 0;
    m->bytes = bytes;
    m->device = (struct block_device){0};
    copy_name(m->device.name, name ? name : "");
    m->device.driver = "memory";
    m->device.block_size = block_size; m->device.block_count = block_count;
    m->device.flags = BLOCK_FLAG_FLUSH | BLOCK_FLAG_SCRATCH | (read_only ? BLOCK_FLAG_READ_ONLY : 0);
    m->device.ops = &memory_ops; m->device.context = m;
    if (!block_register(&m->device)) {
        vmm_free_pages(m->data); m->data = 0; m->device = (struct block_device){0};
        return 0;
    }
    return &m->device;
}
bool block_memory_destroy(struct block_device *d) {
    for (size_t i = 0; i < 2; ++i) {
        struct memory_block *m = &memory_devices[i];
        if (&m->device != d || !m->data) continue;
        if (!block_unregister(d)) return false;
        bool freed = vmm_free_pages(m->data);
        if (!freed) return false;
        m->data = 0; m->device = (struct block_device){0};
        return true;
    }
    return false;
}
