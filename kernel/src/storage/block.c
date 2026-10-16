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

/* ---- memory-backed test device ----
   Optionally models a volatile write cache. With the cache enabled, writes
   are applied to the image immediately (reads see them, like a real cache) but
   remain *pending* until a flush; the first pre-write content of every pending
   block is kept so a simulated power cut can lose any subset of them. A power
   cut can also be injected at a chosen I/O event (a write or a flush), after
   which the device fails all I/O until block_memory_power_cycle(). */
#define MEM_PENDING_MAX 256U
struct memory_block {
    struct block_device device;
    uint8_t *data;
    uint64_t bytes;
    /* volatile-cache model */
    bool cache;
    uint8_t *undo;                      /* MEM_PENDING_MAX pre-write images */
    uint32_t pending[MEM_PENDING_MAX];  /* device blocks, first-write order */
    uint32_t pending_count;
    bool crashed;
    int64_t crash_at;                   /* event index to crash at, -1 = never */
    uint64_t events;                    /* writes and flushes executed since reset */
    const char *crash_phase;
};
static struct memory_block memory_devices[2];
static const char *current_phase = "";
void block_set_phase(const char *phase) { current_phase = phase ? phase : ""; }
const char *block_get_phase(void) { return current_phase; }

/* Returns true when this event must not execute (power already cut or cut now). */
static bool memory_event_blocked(struct memory_block *m) {
    if (m->crashed) return true;
    if (m->crash_at >= 0 && m->events == (uint64_t)m->crash_at) {
        m->crashed = true; m->crash_phase = current_phase;
        return true;
    }
    return false;
}
static enum block_error memory_read(struct block_device *d, uint64_t block, void *buffer) {
    struct memory_block *m = d->context;
    if (m->crashed) return BLOCK_IO;
    uint8_t *source = m->data + block * d->block_size;
    for (uint32_t i = 0; i < d->block_size; ++i) ((uint8_t *)buffer)[i] = source[i];
    return BLOCK_OK;
}
static void memory_make_durable(struct memory_block *m) { m->pending_count = 0; }
static enum block_error memory_write(struct block_device *d, uint64_t block, const void *buffer) {
    struct memory_block *m = d->context;
    if (memory_event_blocked(m)) return BLOCK_IO;
    ++m->events;
    uint8_t *target = m->data + block * d->block_size;
    if (m->cache) {
        uint32_t slot = 0;
        while (slot < m->pending_count && m->pending[slot] != (uint32_t)block) ++slot;
        if (slot == m->pending_count) {
            if (m->pending_count == MEM_PENDING_MAX) {   /* cache eviction: everything written back */
                memory_make_durable(m); slot = 0;
            }
            slot = m->pending_count++;
            m->pending[slot] = (uint32_t)block;
            uint8_t *undo = m->undo + (uint64_t)slot * d->block_size;
            for (uint32_t i = 0; i < d->block_size; ++i) undo[i] = target[i];
        }
    }
    for (uint32_t i = 0; i < d->block_size; ++i) target[i] = ((const uint8_t *)buffer)[i];
    return BLOCK_OK;
}
static enum block_error memory_flush(struct block_device *d) {
    struct memory_block *m = d->context;
    if (memory_event_blocked(m)) return BLOCK_IO;
    ++m->events;
    memory_make_durable(m);
    return BLOCK_OK;
}
static const struct block_ops memory_ops = {memory_read, memory_write, memory_flush};

static struct memory_block *memory_of(struct block_device *d) {
    for (size_t i = 0; i < 2; ++i) if (&memory_devices[i].device == d && memory_devices[i].data) return &memory_devices[i];
    return 0;
}
bool block_memory_enable_cache(struct block_device *d) {
    struct memory_block *m = memory_of(d);
    if (!m || m->cache) return false;
    m->undo = vmm_alloc_pages((size_t)(((uint64_t)MEM_PENDING_MAX * d->block_size + 4095) / 4096));
    if (!m->undo) return false;
    m->cache = true; m->pending_count = 0; m->crashed = false; m->crash_at = -1; m->events = 0;
    return true;
}
void block_memory_crash_after(struct block_device *d, int64_t events) {
    struct memory_block *m = memory_of(d);
    if (!m) return;
    m->crash_at = events; m->events = 0; m->crashed = false; m->crash_phase = "";
}
uint64_t block_memory_events(struct block_device *d) { struct memory_block *m = memory_of(d); return m ? m->events : 0; }
bool block_memory_crashed(struct block_device *d) { struct memory_block *m = memory_of(d); return m && m->crashed; }
const char *block_memory_crash_phase(struct block_device *d) { struct memory_block *m = memory_of(d); return m && m->crash_phase ? m->crash_phase : ""; }
uint32_t block_memory_pending(struct block_device *d) { struct memory_block *m = memory_of(d); return m ? m->pending_count : 0; }

static void memory_revert(struct memory_block *m, uint32_t slot, uint32_t keep_sectors) {
    uint32_t bs = m->device.block_size;
    uint8_t *target = m->data + (uint64_t)m->pending[slot] * bs, *old = m->undo + (uint64_t)slot * bs;
    /* Restore the pre-write image, except for the first keep_sectors 512-byte sectors (torn write). */
    for (uint32_t i = keep_sectors * 512U; i < bs; ++i) target[i] = old[i];
}
void block_memory_power_cycle(struct block_device *d, enum block_power_policy policy, uint32_t parameter) {
    struct memory_block *m = memory_of(d);
    if (!m) return;
    if (m->cache) {
        uint32_t state = parameter * 2654435761U + 12345U;
        for (uint32_t i = 0; i < m->pending_count; ++i) {
            bool keep = false; uint32_t torn = 0;
            switch (policy) {
                case POWER_KEEP_ALL: keep = true; break;
                case POWER_LOSE_ALL: keep = false; break;
                case POWER_KEEP_PREFIX: keep = i < parameter; break;
                case POWER_KEEP_SUFFIX: keep = i + parameter >= m->pending_count; break;
                case POWER_TORN:
                    keep = i < parameter;
                    if (i == parameter && m->device.block_size > 512) torn = m->device.block_size / 512U / 2U;
                    break;
                case POWER_RANDOM:
                    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                    keep = state & 1U;
                    break;
            }
            if (!keep) memory_revert(m, i, torn);
        }
        m->pending_count = 0;
    }
    m->crashed = false; m->crash_at = -1; m->events = 0;
}
bool block_memory_copy(struct block_device *to, struct block_device *from) {
    struct memory_block *a = memory_of(to), *b = memory_of(from);
    if (!a || !b || a == b || a->bytes != b->bytes || to->block_size != from->block_size) return false;
    for (uint64_t i = 0; i < a->bytes; ++i) a->data[i] = b->data[i];
    a->pending_count = 0; a->crashed = false; a->crash_at = -1; a->events = 0;
    return true;
}

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
    m->cache = false; m->undo = 0; m->pending_count = 0; m->crashed = false; m->crash_at = -1; m->events = 0; m->crash_phase = "";
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
        if (m->undo) freed = vmm_free_pages(m->undo) && freed;
        if (!freed) return false;
        m->data = 0; m->undo = 0; m->cache = false; m->device = (struct block_device){0};
        return true;
    }
    return false;
}
