#ifndef ULTRIUM_PCI_H
#define ULTRIUM_PCI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal PCI enumeration through configuration mechanism #1 (ports
   0xCF8/0xCFC). Only bus 0 is scanned; devices behind bridges are not
   discovered. This is sufficient for QEMU Q35 root-bus devices. */
#define PCI_MAX_DEVICES 32U

struct pci_device {
    uint8_t bus, slot, function;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision;
};

void pci_init(void);
size_t pci_device_count(void);
const struct pci_device *pci_device_at(size_t index);
uint32_t pci_read32(const struct pci_device *device, uint8_t offset);
uint16_t pci_read16(const struct pci_device *device, uint8_t offset);
uint8_t pci_read8(const struct pci_device *device, uint8_t offset);
void pci_write32(const struct pci_device *device, uint8_t offset, uint32_t value);
void pci_write16(const struct pci_device *device, uint8_t offset, uint16_t value);

#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_BAR0 0x10
#define PCI_CAPABILITIES 0x34
#define PCI_COMMAND_IO 0x0001
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_BUS_MASTER 0x0004
#define PCI_COMMAND_INTX_DISABLE 0x0400
#define PCI_STATUS_CAPABILITIES 0x0010

/* Decode a memory BAR. Probes its size with decoding disabled and restores
   the original value. Returns false for I/O, unimplemented or malformed BARs. */
bool pci_memory_bar(const struct pci_device *device, unsigned bar,
                    uint64_t *base, uint64_t *size);
#endif
