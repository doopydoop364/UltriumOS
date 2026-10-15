#include <stddef.h>
#include <stdint.h>
#include "pci.h"

static struct pci_device devices[PCI_MAX_DEVICES];
static size_t device_total;

static inline void out32(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    return 0x80000000U | (uint32_t)bus << 16 | (uint32_t)(slot & 31) << 11 |
           (uint32_t)(function & 7) << 8 | (offset & 0xfc);
}
static uint32_t raw_read(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    out32(0xcf8, config_address(bus, slot, function, offset));
    return in32(0xcfc);
}
static void raw_write(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    out32(0xcf8, config_address(bus, slot, function, offset));
    out32(0xcfc, value);
}

uint32_t pci_read32(const struct pci_device *d, uint8_t offset) {
    return raw_read(d->bus, d->slot, d->function, offset);
}
uint16_t pci_read16(const struct pci_device *d, uint8_t offset) {
    return (uint16_t)(pci_read32(d, offset) >> ((offset & 2) * 8));
}
uint8_t pci_read8(const struct pci_device *d, uint8_t offset) {
    return (uint8_t)(pci_read32(d, offset) >> ((offset & 3) * 8));
}
void pci_write32(const struct pci_device *d, uint8_t offset, uint32_t value) {
    raw_write(d->bus, d->slot, d->function, offset, value);
}
void pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value) {
    uint32_t shift = (offset & 2) * 8, current = pci_read32(d, offset & 0xfc);
    current = (current & ~(0xffffU << shift)) | (uint32_t)value << shift;
    pci_write32(d, offset & 0xfc, current);
}

void pci_init(void) {
    device_total = 0;
    for (unsigned slot = 0; slot < 32 && device_total < PCI_MAX_DEVICES; ++slot) {
        uint32_t id0 = raw_read(0, (uint8_t)slot, 0, 0);
        if ((id0 & 0xffff) == 0xffff) continue;
        uint8_t header = (uint8_t)(raw_read(0, (uint8_t)slot, 0, 0x0c) >> 16);
        unsigned functions = (header & 0x80) ? 8 : 1;
        for (unsigned function = 0; function < functions && device_total < PCI_MAX_DEVICES; ++function) {
            uint32_t id = raw_read(0, (uint8_t)slot, (uint8_t)function, 0);
            if ((id & 0xffff) == 0xffff) continue;
            uint32_t class_reg = raw_read(0, (uint8_t)slot, (uint8_t)function, 8);
            struct pci_device *d = &devices[device_total++];
            d->bus = 0; d->slot = (uint8_t)slot; d->function = (uint8_t)function;
            d->vendor = (uint16_t)id; d->device = (uint16_t)(id >> 16);
            d->class_code = (uint8_t)(class_reg >> 24); d->subclass = (uint8_t)(class_reg >> 16);
            d->prog_if = (uint8_t)(class_reg >> 8); d->revision = (uint8_t)class_reg;
        }
    }
}
size_t pci_device_count(void) { return device_total; }
const struct pci_device *pci_device_at(size_t index) { return index < device_total ? &devices[index] : 0; }

bool pci_memory_bar(const struct pci_device *d, unsigned bar, uint64_t *base, uint64_t *size) {
    if (bar >= 6 || !base || !size) return false;
    uint8_t offset = (uint8_t)(PCI_BAR0 + bar * 4);
    uint32_t low = pci_read32(d, offset);
    if (low & 1) return false;                 /* I/O space */
    bool is64 = ((low >> 1) & 3) == 2;
    if (is64 && bar == 5) return false;
    uint32_t high = is64 ? pci_read32(d, (uint8_t)(offset + 4)) : 0;
    uint16_t command = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, (uint16_t)(command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)));
    pci_write32(d, offset, 0xffffffffU);
    uint32_t size_low = pci_read32(d, offset), size_high = 0xffffffffU;
    pci_write32(d, offset, low);
    if (is64) {
        pci_write32(d, (uint8_t)(offset + 4), 0xffffffffU);
        size_high = pci_read32(d, (uint8_t)(offset + 4));
        pci_write32(d, (uint8_t)(offset + 4), high);
    }
    pci_write16(d, PCI_COMMAND, command);
    uint64_t mask = ((uint64_t)size_high << 32) | (size_low & 0xfffffff0U);
    if (!is64) mask |= 0xffffffff00000000ULL;
    if (!(size_low & 0xfffffff0U) && !is64) return false;
    uint64_t length = ~mask + 1;
    uint64_t address = ((uint64_t)high << 32) | (low & 0xfffffff0U);
    if (!length || !address || (length & (length - 1)) || address + length < address) return false;
    *base = address; *size = length;
    return true;
}
