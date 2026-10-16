#include <stddef.h>
#include <stdint.h>
#include "virtio_blk.h"
#include "block.h"
#include "pci/pci.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

/* DMA ownership: each device holds three PMM handles for its lifetime. The
   ring frame holds descriptor table, available ring and used ring at
   separate aligned offsets (queue size 16). The request frame holds the
   16-byte request header and the 1-byte status. The data frame is a bounce
   buffer for one logical block (<= 4096 bytes). All three are ordinary
   write-back RAM; x86 DMA is cache-coherent, so ordering is provided by
   explicit fences. Frames are never released while the device may still
   access them: on timeout the device is reset first, and if the reset is not
   confirmed the frames are intentionally leaked and the device stays dead. */

#define VIRTIO_VENDOR 0x1af4
#define VIRTIO_BLK_MODERN_ID 0x1042
#define VIRTIO_BLK_TRANSITIONAL_ID 0x1001
#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR 3
#define CAP_DEVICE 4
#define STATUS_ACK 1
#define STATUS_DRIVER 2
#define STATUS_DRIVER_OK 4
#define STATUS_FEATURES_OK 8
#define STATUS_FAILED 128
#define F_RO (1U << 5)
#define F_BLK_SIZE (1U << 6)
#define F_FLUSH (1U << 9)
#define F_VERSION_1 1U /* feature word 1, bit 0 (feature 32) */
#define QUEUE_SIZE 16U
#define DESC_NEXT 1U
#define DESC_WRITE 2U
#define T_IN 0U
#define T_OUT 1U
#define T_FLUSH 4U
#define T_GET_ID 8U
#define POLL_LIMIT 200000000U
#define MAX_DEVICES 2U

struct virtio_dev {
    struct block_device block;
    volatile uint8_t *common, *notify, *device_cfg;
    uint32_t notify_multiplier;
    uint16_t notify_offset;
    struct pmm_page_handle ring_frame, request_frame, data_frame;
    uint8_t *ring, *request, *data;
    uint16_t avail_index, last_used;
    bool in_use, dead;
};
static struct virtio_dev devices[MAX_DEVICES];
static const char *last_error = "no virtio-blk device probed";
static unsigned name_counter;

const char *virtio_blk_last_error(void) { return last_error; }

static uint8_t r8(volatile uint8_t *base, unsigned o) { return *(volatile uint8_t *)(base + o); }
static uint16_t r16(volatile uint8_t *base, unsigned o) { return *(volatile uint16_t *)(base + o); }
static uint32_t r32(volatile uint8_t *base, unsigned o) { return *(volatile uint32_t *)(base + o); }
static void w8(volatile uint8_t *base, unsigned o, uint8_t v) { *(volatile uint8_t *)(base + o) = v; }
static void w16(volatile uint8_t *base, unsigned o, uint16_t v) { *(volatile uint16_t *)(base + o) = v; }
static void w32(volatile uint8_t *base, unsigned o, uint32_t v) { *(volatile uint32_t *)(base + o) = v; }
static void w64(volatile uint8_t *base, unsigned o, uint64_t v) {
    w32(base, o, (uint32_t)v); w32(base, o + 4, (uint32_t)(v >> 32));
}
static void fence(void) { __asm__ volatile ("mfence" ::: "memory"); }

#define C_DEVICE_FEATURE_SELECT 0
#define C_DEVICE_FEATURE 4
#define C_DRIVER_FEATURE_SELECT 8
#define C_DRIVER_FEATURE 12
#define C_NUM_QUEUES 18
#define C_STATUS 20
#define C_QUEUE_SELECT 22
#define C_QUEUE_SIZE 24
#define C_QUEUE_ENABLE 28
#define C_QUEUE_NOTIFY_OFF 30
#define C_QUEUE_DESC 32
#define C_QUEUE_DRIVER 40
#define C_QUEUE_DEVICE 48

/* Reset and confirm: the device reads status 0 once it has stopped. */
static bool device_reset(volatile uint8_t *common) {
    w8(common, C_STATUS, 0);
    for (unsigned i = 0; i < 1000000; ++i) {
        if (r8(common, C_STATUS) == 0) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void release_frames(struct virtio_dev *d) {
    pmm_page_release(&d->ring_frame);
    pmm_page_release(&d->request_frame);
    pmm_page_release(&d->data_frame);
    d->ring = d->request = d->data = 0;
}

static void mark_dead(struct virtio_dev *d) {
    d->dead = true;
    d->block.flags |= BLOCK_FLAG_DEAD;
    if (device_reset(d->common)) release_frames(d);
    /* Otherwise the DMA frames are deliberately retained forever. */
}

static enum block_error submit(struct virtio_dev *d, uint32_t type, uint64_t sector,
                               uint32_t data_length, bool device_writes) {
    if (d->dead) return BLOCK_NO_DEVICE;
    volatile uint8_t *header = d->request;
    volatile uint32_t *h32 = (volatile uint32_t *)header;
    h32[0] = type; h32[1] = 0;
    *(volatile uint64_t *)(header + 8) = sector;
    header[16] = 0xff; /* status; the device overwrites it */
    uint64_t request_physical = d->request_frame.physical;
    volatile uint8_t *desc = d->ring;
    /* descriptor: addr u64, len u32, flags u16, next u16 */
    #define SET_DESC(i, address, length, flags, next) do { \
        *(volatile uint64_t *)(desc + (i) * 16) = (address); \
        *(volatile uint32_t *)(desc + (i) * 16 + 8) = (length); \
        *(volatile uint16_t *)(desc + (i) * 16 + 12) = (flags); \
        *(volatile uint16_t *)(desc + (i) * 16 + 14) = (next); } while (0)
    SET_DESC(0, request_physical, 16, DESC_NEXT, 1);
    if (data_length) {
        SET_DESC(1, d->data_frame.physical, data_length,
                 (uint16_t)(DESC_NEXT | (device_writes ? DESC_WRITE : 0)), 2);
    } else {
        SET_DESC(1, request_physical + 16, 1, DESC_WRITE, 0);
    }
    if (data_length) SET_DESC(2, request_physical + 16, 1, DESC_WRITE, 0);
    volatile uint8_t *avail = d->ring + 256, *used_ring = d->ring + 512;
    *(volatile uint16_t *)(avail + 4 + (d->avail_index % QUEUE_SIZE) * 2) = 0;
    fence();
    d->avail_index = (uint16_t)(d->avail_index + 1);
    *(volatile uint16_t *)(avail + 2) = d->avail_index;
    fence();
    w16(d->notify, (unsigned)d->notify_offset * d->notify_multiplier, 0);
    for (uint32_t spin = 0; spin < POLL_LIMIT; ++spin) {
        if (*(volatile uint16_t *)(used_ring + 2) != d->last_used) {
            fence();
            d->last_used = (uint16_t)(d->last_used + 1);
            uint8_t status = header[16];
            if (status == 0) return BLOCK_OK;
            return status == 2 ? BLOCK_UNSUPPORTED : BLOCK_IO;
        }
        __asm__ volatile ("pause");
    }
    mark_dead(d);
    return BLOCK_TIMEOUT;
}

static enum block_error blk_read(struct block_device *b, uint64_t block, void *buffer) {
    struct virtio_dev *d = b->context;
    enum block_error error = submit(d, T_IN, block * (b->block_size / 512U), b->block_size, true);
    if (error != BLOCK_OK) return error;
    for (uint32_t i = 0; i < b->block_size; ++i) ((uint8_t *)buffer)[i] = d->data[i];
    return BLOCK_OK;
}
static enum block_error blk_write(struct block_device *b, uint64_t block, const void *buffer) {
    struct virtio_dev *d = b->context;
    if (d->dead) return BLOCK_NO_DEVICE;
    for (uint32_t i = 0; i < b->block_size; ++i) d->data[i] = ((const uint8_t *)buffer)[i];
    return submit(d, T_OUT, block * (b->block_size / 512U), b->block_size, false);
}
static enum block_error blk_flush(struct block_device *b) {
    struct virtio_dev *d = b->context;
    return submit(d, T_FLUSH, 0, 0, false);
}
static const struct block_ops virtio_ops = {blk_read, blk_write, blk_flush};

static bool fail(const char *reason) { last_error = reason; return false; }

struct caps {
    bool seen[5];
    uint8_t bar[5];
    uint32_t offset[5], length[5], notify_multiplier;
};

static bool find_caps(const struct pci_device *p, struct caps *c) {
    if (!(pci_read16(p, PCI_STATUS) & PCI_STATUS_CAPABILITIES)) return false;
    uint8_t pointer = pci_read8(p, PCI_CAPABILITIES) & 0xfc;
    for (unsigned guard = 0; pointer >= 0x40 && guard < 48; ++guard) {
        uint8_t id = pci_read8(p, pointer), next = pci_read8(p, (uint8_t)(pointer + 1)) & 0xfc;
        if (id == 0x09 && pci_read8(p, (uint8_t)(pointer + 2)) >= 16) {
            uint8_t type = pci_read8(p, (uint8_t)(pointer + 3));
            if (type >= 1 && type <= 4 && !c->seen[type]) {
                c->seen[type] = true;
                c->bar[type] = pci_read8(p, (uint8_t)(pointer + 4));
                c->offset[type] = pci_read32(p, (uint8_t)(pointer + 8));
                c->length[type] = pci_read32(p, (uint8_t)(pointer + 12));
                if (type == CAP_NOTIFY) c->notify_multiplier = pci_read32(p, (uint8_t)(pointer + 16));
            }
        }
        pointer = next;
    }
    return c->seen[CAP_COMMON] && c->seen[CAP_NOTIFY] && c->seen[CAP_ISR] && c->seen[CAP_DEVICE];
}

static volatile uint8_t *map_cap(const struct pci_device *p, const struct caps *c, unsigned type,
                                 uint32_t minimum) {
    uint64_t base, size;
    if (c->bar[type] > 5 || !pci_memory_bar(p, c->bar[type], &base, &size)) return 0;
    uint64_t end = (uint64_t)c->offset[type] + c->length[type];
    if (c->length[type] < minimum || end < c->offset[type] || end > size) return 0;
    return vmm_map_mmio(base + c->offset[type], c->length[type]);
}

static bool bring_up(const struct pci_device *p, struct virtio_dev *d) {
    struct caps c = {0};
    if (!find_caps(p, &c)) return fail("device has no modern virtio capabilities (legacy-only is unsupported)");
    d->common = map_cap(p, &c, CAP_COMMON, 56);
    d->notify = map_cap(p, &c, CAP_NOTIFY, 2);
    d->device_cfg = map_cap(p, &c, CAP_DEVICE, 24);
    if (!d->common || !d->notify || !d->device_cfg) return fail("could not map virtio MMIO capabilities");
    d->notify_multiplier = c.notify_multiplier;
    uint16_t command = pci_read16(p, PCI_COMMAND);
    pci_write16(p, PCI_COMMAND, (uint16_t)(command | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INTX_DISABLE));

    if (!device_reset(d->common)) return fail("device reset did not complete");
    w8(d->common, C_STATUS, STATUS_ACK);
    w8(d->common, C_STATUS, STATUS_ACK | STATUS_DRIVER);
    w32(d->common, C_DEVICE_FEATURE_SELECT, 0);
    uint32_t low = r32(d->common, C_DEVICE_FEATURE);
    w32(d->common, C_DEVICE_FEATURE_SELECT, 1);
    uint32_t high = r32(d->common, C_DEVICE_FEATURE);
    if (!(high & F_VERSION_1)) { w8(d->common, C_STATUS, STATUS_FAILED); return fail("device does not offer VIRTIO_F_VERSION_1"); }
    uint32_t accept = low & (F_RO | F_BLK_SIZE | F_FLUSH);
    w32(d->common, C_DRIVER_FEATURE_SELECT, 0); w32(d->common, C_DRIVER_FEATURE, accept);
    w32(d->common, C_DRIVER_FEATURE_SELECT, 1); w32(d->common, C_DRIVER_FEATURE, F_VERSION_1);
    w8(d->common, C_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(r8(d->common, C_STATUS) & STATUS_FEATURES_OK)) { w8(d->common, C_STATUS, STATUS_FAILED); return fail("device rejected the feature set"); }

    w16(d->common, C_QUEUE_SELECT, 0);
    uint16_t maximum = r16(d->common, C_QUEUE_SIZE);
    if (maximum < QUEUE_SIZE) { (void)device_reset(d->common); return fail("queue 0 is smaller than 16 entries"); }
    if (!pmm_page_alloc(&d->ring_frame) || !pmm_page_alloc(&d->request_frame) || !pmm_page_alloc(&d->data_frame)) {
        release_frames(d); (void)device_reset(d->common); return fail("out of physical frames for DMA");
    }
    d->ring = vmm_direct_map(d->ring_frame.physical);
    d->request = vmm_direct_map(d->request_frame.physical);
    d->data = vmm_direct_map(d->data_frame.physical);
    if (!d->ring || !d->request || !d->data) { (void)device_reset(d->common); release_frames(d); return fail("DMA frames are not directly mapped"); }
    for (unsigned i = 0; i < 4096; ++i) { d->ring[i] = 0; d->request[i] = 0; d->data[i] = 0; }
    d->avail_index = d->last_used = 0;
    w16(d->common, C_QUEUE_SIZE, QUEUE_SIZE);
    w64(d->common, C_QUEUE_DESC, d->ring_frame.physical);
    w64(d->common, C_QUEUE_DRIVER, d->ring_frame.physical + 256);
    w64(d->common, C_QUEUE_DEVICE, d->ring_frame.physical + 512);
    d->notify_offset = r16(d->common, C_QUEUE_NOTIFY_OFF);
    /* The notify address must lie inside the mapped notify capability. */
    if ((uint64_t)d->notify_offset * d->notify_multiplier + 2 > c.length[CAP_NOTIFY]) {
        (void)device_reset(d->common); release_frames(d); return fail("queue notify offset outside the notify capability");
    }
    w16(d->common, C_QUEUE_ENABLE, 1);
    ((volatile uint16_t *)(d->ring + 256))[0] = 1;  /* avail flags: no interrupts */
    w8(d->common, C_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

    uint64_t capacity_sectors = (uint64_t)r32(d->device_cfg, 0) | (uint64_t)r32(d->device_cfg, 4) << 32;
    uint32_t block_size = (accept & F_BLK_SIZE) ? r32(d->device_cfg, 20) : 512U;
    if (block_size < 512 || block_size > BLOCK_MAX_SIZE || (block_size & (block_size - 1))) {
        (void)device_reset(d->common); release_frames(d); return fail("unsupported logical block size");
    }
    uint64_t blocks = capacity_sectors / (block_size / 512U);
    if (!blocks) { (void)device_reset(d->common); release_frames(d); return fail("device reports zero capacity"); }

    d->block.driver = "virtio-blk";
    d->block.block_size = block_size; d->block.block_count = blocks;
    d->block.flags = ((accept & F_RO) ? BLOCK_FLAG_READ_ONLY : 0) | ((accept & F_FLUSH) ? BLOCK_FLAG_FLUSH : 0);
    d->block.ops = &virtio_ops; d->block.context = d;
    /* Identify (serial) through VIRTIO_BLK_T_GET_ID. Failure is non-fatal. */
    if (submit(d, T_GET_ID, 0, BLOCK_SERIAL_MAX, true) == BLOCK_OK) {
        unsigned n = 0;
        for (; n < BLOCK_SERIAL_MAX && d->data[n] && d->data[n] >= 32 && d->data[n] < 127; ++n) d->block.serial[n] = (char)d->data[n];
        d->block.serial[n] = 0;
        /* Only a disk whose serial starts with this marker may be formatted. */
        static const char marker[] = "ULTRIUM-SCRATCH";
        unsigned m = 0;
        while (marker[m] && d->block.serial[m] == marker[m]) ++m;
        if (!marker[m]) d->block.flags |= BLOCK_FLAG_SCRATCH;
    }
    return true;
}

size_t virtio_blk_probe(void) {
    size_t registered = 0;
    for (size_t i = 0; i < pci_device_count(); ++i) {
        const struct pci_device *p = pci_device_at(i);
        if (p->vendor != VIRTIO_VENDOR || (p->device != VIRTIO_BLK_MODERN_ID && p->device != VIRTIO_BLK_TRANSITIONAL_ID)) continue;
        struct virtio_dev *d = 0;
        for (unsigned k = 0; k < MAX_DEVICES; ++k) if (!devices[k].in_use) { d = &devices[k]; break; }
        if (!d) { last_error = "too many virtio-blk devices"; break; }
        *d = (struct virtio_dev){0};
        d->in_use = true;
        if (!bring_up(p, d)) { d->in_use = false; continue; }
        d->block.name[0] = 'v'; d->block.name[1] = 'd';
        d->block.name[2] = (char)('a' + name_counter % 26); d->block.name[3] = 0;
        ++name_counter;
        if (!block_register(&d->block)) {
            (void)device_reset(d->common); release_frames(d); d->in_use = false;
            last_error = "block registry rejected the device"; continue;
        }
        last_error = "ok";
        ++registered;
    }
    return registered;
}
