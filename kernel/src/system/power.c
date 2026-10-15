#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "power.h"
#include "terminal/terminal.h"

/* ACPI soft-off (S5). Nothing here is hardcoded to a chipset: the PM1 control
   port comes from the FADT and SLP_TYP from the DSDT \_S5 package. Only
   system-I/O PM1 blocks are supported; anything else fails clearly. */

#define FADT_DSDT 40
#define FADT_PM1A_CNT 64
#define FADT_PM1B_CNT 68
#define FADT_PM1_CNT_LEN 89
#define FADT_X_DSDT 140
#define FADT_X_PM1A_CNT 172
#define FADT_X_PM1B_CNT 184
#define FADT_MIN_LENGTH 129 /* through PM1_CNT_LEN and legacy fields */
#define TABLE_LIMIT (16u * 1024u * 1024u)
#define PM1_SCI_EN 0x0001u
#define PM1_SLP_EN 0x2000u

static struct {
    const struct limine_memmap_response *memmap;
    uint64_t hhdm;
    bool available;
    const char *reason;
    uint8_t acpi_revision;
    uint16_t pm1a, pm1b;
    uint8_t slp_typ_a, slp_typ_b;
    uint64_t rsdp;
} state = {0, 0, false, "ACPI discovery has not run", 0, 0, 0, 0, 0, 0};

static uint16_t in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static void out16(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

/* A range is readable only if one Limine entry of an ACPI-bearing type fully
   contains it; the HHDM is not assumed to cover arbitrary physical memory. */
static bool range_ok(uint64_t physical, uint64_t length) {
    if (!state.memmap || length == 0 || physical + length < physical) return false;
    for (uint64_t i = 0; i < state.memmap->entry_count; ++i) {
        const struct limine_memmap_entry *e = state.memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_ACPI_RECLAIMABLE && e->type != LIMINE_MEMMAP_ACPI_NVS &&
            e->type != LIMINE_MEMMAP_RESERVED_MAPPED) continue;
        if (e->base + e->length < e->base) continue;
        if (physical >= e->base && physical + length <= e->base + e->length) return true;
    }
    return false;
}
static const uint8_t *map(uint64_t physical, uint64_t length) {
    return range_ok(physical, length) ? (const uint8_t *)(uintptr_t)(state.hhdm + physical) : 0;
}
static bool checksum_ok(const uint8_t *data, uint64_t length) {
    uint8_t sum = 0;
    for (uint64_t i = 0; i < length; ++i) sum = (uint8_t)(sum + data[i]);
    return sum == 0;
}
/* Maps an SDT header, then the whole table once its length is bounded. */
static const uint8_t *table(uint64_t physical, const char *signature) {
    const uint8_t *header = map(physical, 36);
    if (!header) return 0;
    uint32_t length = le32(header + 4);
    if (length < 36 || length > TABLE_LIMIT) return 0;
    const uint8_t *whole = map(physical, length);
    if (!whole || !checksum_ok(whole, length)) return 0;
    for (int i = 0; i < 4; ++i)
        if (signature && whole[i] != (uint8_t)signature[i]) return 0;
    return whole;
}

/* Reads \_S5_ = Package { SLP_TYPa, SLP_TYPb, ... } from AML. The scan is
   bounded by the DSDT length; the encoding is validated, not assumed. */
static bool parse_s5(const uint8_t *dsdt, uint32_t length, uint8_t *a, uint8_t *b) {
    for (uint32_t i = 36; i + 4 < length; ++i) {
        if (dsdt[i] != '_' || dsdt[i + 1] != 'S' || dsdt[i + 2] != '5' || dsdt[i + 3] != '_') continue;
        if (i == 0 || !(dsdt[i - 1] == 0x08 || (i >= 2 && dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\'))) continue;
        uint32_t p = i + 4;
        if (p + 3 >= length || dsdt[p] != 0x12) continue; /* PackageOp */
        ++p;
        uint32_t lead_bytes = (uint32_t)(dsdt[p] >> 6) + 1; /* PkgLength encoding size */
        p += lead_bytes;
        if (p + 1 >= length) continue;
        uint8_t elements = dsdt[p++];
        if (elements < 2) continue;
        uint8_t values[2];
        for (int k = 0; k < 2; ++k) {
            if (p >= length) return false;
            if (dsdt[p] == 0x0a) { /* BytePrefix */
                if (p + 1 >= length) return false;
                values[k] = dsdt[p + 1]; p += 2;
            } else if (dsdt[p] == 0x00 || dsdt[p] == 0x01) { /* ZeroOp / OneOp */
                values[k] = dsdt[p]; ++p;
            } else return false;
        }
        if (values[0] > 7 || values[1] > 7) return false;
        *a = values[0]; *b = values[1];
        return true;
    }
    return false;
}

static bool fail(const char *reason) { state.reason = reason; return false; }

static bool discover(uint64_t rsdp_physical) {
    if (!rsdp_physical) return fail("firmware provided no ACPI RSDP");
    const uint8_t *rsdp = map(rsdp_physical, 36);
    if (!rsdp) return fail("RSDP is outside ACPI-mapped memory");
    static const char sig[8] = {'R','S','D',' ','P','T','R',' '};
    for (int i = 0; i < 8; ++i) if (rsdp[i] != (uint8_t)sig[i]) return fail("RSDP signature is invalid");
    if (!checksum_ok(rsdp, 20)) return fail("RSDP checksum is invalid");
    state.acpi_revision = rsdp[15];
    bool use_xsdt = state.acpi_revision >= 2;
    if (use_xsdt && (le32(rsdp + 20) < 36 || !checksum_ok(rsdp, le32(rsdp + 20))))
        return fail("RSDP extended checksum is invalid");
    uint64_t root = use_xsdt ? le64(rsdp + 24) : le32(rsdp + 16);
    const uint8_t *sdt = table(root, use_xsdt ? "XSDT" : "RSDT");
    if (!sdt) return fail("root system description table is invalid");
    uint32_t entry_size = use_xsdt ? 8 : 4, count = (le32(sdt + 4) - 36) / entry_size;
    const uint8_t *fadt = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t address = use_xsdt ? le64(sdt + 36 + i * 8) : le32(sdt + 36 + i * 4);
        const uint8_t *candidate = table(address, "FACP");
        if (candidate) { fadt = candidate; break; }
    }
    if (!fadt) return fail("no valid FADT found");
    uint32_t fadt_length = le32(fadt + 4);
    if (fadt_length < FADT_MIN_LENGTH) return fail("FADT is too short");

    uint64_t pm1a = le32(fadt + FADT_PM1A_CNT), pm1b = le32(fadt + FADT_PM1B_CNT);
    if (fadt_length >= FADT_X_PM1B_CNT + 12) {
        const uint8_t *xa = fadt + FADT_X_PM1A_CNT, *xb = fadt + FADT_X_PM1B_CNT;
        if (le64(xa + 4)) {
            if (xa[0] != 1) return fail("PM1a control block is not in system I/O space");
            pm1a = le64(xa + 4);
        }
        if (le64(xb + 4)) {
            if (xb[0] != 1) return fail("PM1b control block is not in system I/O space");
            pm1b = le64(xb + 4);
        }
    }
    if (!pm1a || pm1a > 0xfffe || pm1b > 0xfffe) return fail("PM1 control port is missing or out of range");
    if (fadt[FADT_PM1_CNT_LEN] < 2) return fail("PM1 control block is shorter than 2 bytes");
    state.pm1a = (uint16_t)pm1a; state.pm1b = (uint16_t)pm1b;

    uint64_t dsdt_address = le32(fadt + FADT_DSDT);
    if (fadt_length >= FADT_X_DSDT + 8 && le64(fadt + FADT_X_DSDT)) dsdt_address = le64(fadt + FADT_X_DSDT);
    const uint8_t *dsdt = table(dsdt_address, "DSDT");
    if (!dsdt) return fail("DSDT is missing or invalid");
    if (!parse_s5(dsdt, le32(dsdt + 4), &state.slp_typ_a, &state.slp_typ_b))
        return fail("DSDT has no parseable \\_S5 sleep object (S5 unsupported)");
    state.available = true;
    state.reason = "ACPI S5 via PM1 control";
    return true;
}

void power_init(const struct limine_memmap_response *memmap, uint64_t hhdm, uint64_t rsdp_virtual) {
    state.memmap = memmap; state.hhdm = hhdm; state.available = false;
    /* Limine base revision 4+ returns the RSDP as an HHDM virtual address. */
    state.rsdp = rsdp_virtual >= hhdm && rsdp_virtual ? rsdp_virtual - hhdm : 0;
    if (rsdp_virtual && !state.rsdp) { state.reason = "RSDP address is below the direct map"; return; }
    (void)discover(state.rsdp);
}
bool power_available(void) { return state.available; }

static void write_hex64(uint64_t value) {
    char text[19] = "0x";
    int digits = 1;
    for (uint64_t t = value >> 4; t; t >>= 4) ++digits;
    for (int i = 0; i < digits; ++i) text[2 + i] = "0123456789abcdef"[(value >> (4 * (digits - 1 - i))) & 15];
    text[2 + digits] = 0;
    terminal_write(text);
}
static void write_hex(uint32_t value) {
    char text[11] = "0x";
    int digits = 0;
    for (uint32_t t = value; t; t >>= 4) ++digits;
    if (!digits) digits = 1;
    for (int i = 0; i < digits; ++i) text[2 + i] = "0123456789abcdef"[(value >> (4 * (digits - 1 - i))) & 15];
    text[2 + digits] = 0;
    terminal_write(text);
}

static uint64_t containing_type(uint64_t physical) {
    if (!state.memmap) return 0xffff;
    for (uint64_t i = 0; i < state.memmap->entry_count; ++i) {
        const struct limine_memmap_entry *e = state.memmap->entries[i];
        if (physical >= e->base && physical - e->base < e->length) return e->type;
    }
    return 0xffff; /* not covered by any entry */
}

void power_status(void) {
    terminal_write("Power-off mechanism: ACPI S5 soft-off through the FADT PM1 control block\n");
    terminal_write("Hardware support: ");
    terminal_write(state.available ? "available\n" : "unavailable\n");
    terminal_write("Detail: "); terminal_write(state.reason); terminal_write("\n");
    terminal_write("RSDP physical address: "); write_hex64(state.rsdp);
    terminal_write("; memory-map type: "); write_hex64(containing_type(state.rsdp)); terminal_write("\n");
    if (!state.available) {
        terminal_write("The shutdown command will report failure and keep the system running.\n");
        return;
    }
    terminal_write("ACPI revision: "); write_hex(state.acpi_revision);
    terminal_write("\nPM1a control port: "); write_hex(state.pm1a);
    terminal_write("\nPM1b control port: "); if (state.pm1b) write_hex(state.pm1b); else terminal_write("none");
    terminal_write("\nSLP_TYP (a/b): "); write_hex(state.slp_typ_a); terminal_write(" / "); write_hex(state.slp_typ_b);
    terminal_write("\nACPI enabled (SCI_EN): ");
    terminal_write((in16(state.pm1a) & PM1_SCI_EN) ? "yes\n" : "no; shutdown will be refused\n");
    terminal_write("Persistent filesystems to synchronize: none mounted\n");
}

static uint64_t save_flags_and_disable(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void power_shutdown(void) {
    terminal_write("Shutting down Ultrium OS...\n");
    if (!state.available) {
        terminal_write("Shutdown failed: "); terminal_write(state.reason);
        terminal_write(".\nThe system is still running.\n");
        return;
    }
    /* Orderly sequence. Command intake stops with interrupts masked. Writable
       persistent filesystems and block devices are synced here once any
       exist; today only volatile RAMFS is mounted. */
    uint64_t flags = save_flags_and_disable();
    terminal_write("No persistent filesystems are mounted; nothing to synchronize.\n");
    if (!(in16(state.pm1a) & PM1_SCI_EN)) {
        terminal_write("Shutdown failed: ACPI mode is not enabled (SCI_EN clear); not enabling it unverified.\n");
        if (flags & 0x200) __asm__ volatile ("sti");
        return;
    }
    terminal_write("Requesting ACPI S5 power-off.\n");
    uint16_t value = (uint16_t)((in16(state.pm1a) & ~0x1c00u) | PM1_SLP_EN | ((uint16_t)state.slp_typ_a << 10));
    out16(state.pm1a, value);
    if (state.pm1b)
        out16(state.pm1b, (uint16_t)((in16(state.pm1b) & ~0x1c00u) | PM1_SLP_EN | ((uint16_t)state.slp_typ_b << 10)));
    for (volatile uint32_t spin = 0; spin < 50000000u; ++spin) __asm__ volatile ("pause");
    terminal_write("Shutdown failed: the platform did not power off.\nThe system is still running.\n");
    if (flags & 0x200) __asm__ volatile ("sti");
}
