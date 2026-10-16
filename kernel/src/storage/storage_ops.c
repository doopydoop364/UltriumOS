#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "storage_ops.h"
#include "storage_cmd.h"
#include "block.h"
#include "fs/vfs.h"
#include "fs/ultriumfs.h"
#include "fs/ufs2_fs.h"
#include "fs/ufs2_check.h"
#include "system/power.h"
#include "terminal/terminal.h"

static const struct vfs_mount_ops ufs_mount_ops = {ufs_sync, ufs_unmount, ufs_mounted};
static const struct vfs_mount_ops ufs2_mount_ops = {ufs2_fs_sync, ufs2_fs_unmount, ufs2_fs_mounted};

/* ---- format detection (block 0, or the last block for a damaged v2 copy A) ---- */
enum storage_format storage_probe(struct block_device *d) {
    static uint8_t probe[4096];
    if (!d || d->block_size < 512 || 4096 % d->block_size) return STORAGE_UNKNOWN;
    uint32_t per = 4096 / d->block_size;
    uint64_t cap = d->block_count / per;
    if (cap > (1U << 20)) cap = 1U << 20;
    if (block_read(d, 0, per, probe, sizeof(probe)) == BLOCK_OK) {
        if (!__builtin_memcmp(probe, "ULTRIUMF", 8)) return STORAGE_V1;
        if (!__builtin_memcmp(probe, "ULTRIUM2", 8)) return STORAGE_V2;
    }
    if (cap >= 512 && block_read(d, (cap - 1) * per, per, probe, sizeof(probe)) == BLOCK_OK && !__builtin_memcmp(probe, "ULTRIUM2", 8)) return STORAGE_V2;
    return STORAGE_UNKNOWN;
}
bool storage_volume_status(struct storage_volume *v) {
    if (ufs2_fs_mounted()) {
        struct ufs2_fs_info i;
        if (!ufs2_fs_get_info(&i)) return false;
        *v = (struct storage_volume){2, i.read_only, i.faulted, i.unclean, i.repair_recommended, i.orphans};
        return true;
    }
    if (ufs_mounted()) {
        struct ufs_info i;
        if (!ufs_get_info(&i)) return false;
        *v = (struct storage_volume){1, i.read_only, i.faulted, i.unclean_mount, false, i.orphan_inodes};
        return true;
    }
    return false;
}
static bool any_volume_mounted(void) { return ufs_mounted() || ufs2_fs_mounted(); }

static size_t text_length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static bool same(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static bool flag(const char *word, const char *short_name, const char *long_name) {
    return ush_word_equal(word, short_name) || ush_word_equal(word, long_name);
}
static void write_size(uint64_t bytes) {
    if (bytes >= (1ULL << 20)) { storage_write_number(bytes >> 20); terminal_write(" MiB"); }
    else if (bytes >= 1024) { storage_write_number(bytes >> 10); terminal_write(" KiB"); }
    else { storage_write_number(bytes); terminal_write(" B"); }
}
static bool ufs_mount_path(char *out, size_t capacity) {   /* where the UltriumFS volume is mounted */
    struct vfs_mount_info info;
    for (size_t i = 0; vfs_mount_info_at(i, &info); ++i)
        if (info.filesystem[0] == 'U' && info.filesystem[1] == 'l') {   /* "UltriumFS 1" / "UltriumFS 2" */
            size_t n = text_length(info.path);
            if (n >= capacity) return false;
            for (size_t k = 0; k <= n; ++k) out[k] = info.path[k];
            return true;
        }
    return false;
}
static bool path_within(const char *base, const char *path) {
    size_t n = text_length(base);
    for (size_t i = 0; i < n; ++i) if (path[i] != base[i]) return false;
    return path[n] == 0 || path[n] == '/' || n == 1;
}

/* ---- format ---- */
static char pending_device[BLOCK_NAME_MAX + 1U];

static enum vfs_error format_precheck(struct block_device *d) {
    if (!d) { terminal_write("No such block device. See 'ultrium storage devices'.\n"); return VFS_NOT_FOUND; }
    if (d->holders || any_volume_mounted()) { terminal_write("Refusing: the device or a volume is mounted/in use. Unmount it first.\n"); return VFS_BUSY; }
    if (!(d->flags & BLOCK_FLAG_SCRATCH)) {
        terminal_write("Refusing: not a disposable scratch disk (virtio serial must start with ULTRIUM-SCRATCH).\n");
        return VFS_DENIED;
    }
    if (d->flags & BLOCK_FLAG_READ_ONLY) { terminal_write("Refusing: the device is read-only.\n"); return VFS_READ_ONLY; }
    return VFS_OK;
}
static enum ush_result do_format(const char *name) {
    struct block_device *d = block_find(name);
    enum vfs_error e = format_precheck(d);   /* re-checked: state may change before confirmation */
    if (e != VFS_OK) return USH_FS_ERROR;
    terminal_write("Formatting "); terminal_write(name); terminal_write(" as UltriumFS 2 (journaled, extent-based)...\n");
    e = ufs2_fs_format(d, "ULTRIUM", 0);
    if (e == VFS_UNSUPPORTED) { terminal_write("Refusing: the device has no flush support, so a journaled filesystem cannot be made durable on it.\n"); return USH_FS_ERROR; }
    if (e != VFS_OK) return ush_fs_failure(e);
    terminal_write("Formatted. Mount it with 'ultrium storage mount "); terminal_write(name); terminal_write(" <folder>'.\n");
    return USH_OK;
}
static void format_answer(bool yes) {
    if (yes) (void)do_format(pending_device); else terminal_write("Format cancelled.\n");
}

/* ---- mount / unmount ---- */
static void print_findings(const struct ufs2_report *r, unsigned limit) {
    static const char *const names[3] = {"repairable", "orphan", "STRUCTURAL"};
    for (uint32_t i = 0; i < r->stored && i < limit; ++i) {
        terminal_write("  ["); terminal_write(names[r->item[i].cls]); terminal_write("] "); terminal_write(ufs2_finding_name(r->item[i].code));
        if (r->item[i].inode) { terminal_write(" (inode "); storage_write_number(r->item[i].inode); terminal_write(")"); }
        if (r->item[i].block) { terminal_write(" (block "); storage_write_number(r->item[i].block); terminal_write(")"); }
        terminal_write("\n");
    }
}
static enum ush_result do_mount(const char *device, const char *argument, bool read_only) {
    struct block_device *d = block_find(device);
    if (!d) { terminal_write("No such block device. See 'ultrium storage devices'.\n"); return USH_FS_ERROR; }
    char path[VFS_PATH_MAX + 1U];
    enum vfs_error e = vfs_resolve_path(argument, ush_cwd(), path, sizeof(path));
    if (e != VFS_OK) return ush_fs_failure(e);
    if (any_volume_mounted()) { terminal_write("An UltriumFS volume is already mounted (one at a time).\n"); return USH_FS_ERROR; }
    enum storage_format kind = storage_probe(d);
    if (kind == STORAGE_UNKNOWN) {
        terminal_write("Mount refused: no UltriumFS volume found on this device. Nothing was modified.\n");
        terminal_write("Scratch disks are formatted explicitly with 'ultrium storage format'.\n");
        return USH_FS_ERROR;
    }
    bool ro = false;
    const char *fsname;
    if (kind == STORAGE_V1) {
        /* Compatibility policy: UltriumFS 1 has no journal, so it is only ever mounted read-only and never written. */
        e = ufs_mount(d, true);
        if (e != VFS_OK) {
            terminal_write(e == VFS_UNSUPPORTED ? "Mount refused: unsupported UltriumFS 1 feature.\n" : "Mount refused: the UltriumFS 1 structure failed verification. Nothing was modified.\n");
            return e == VFS_CORRUPT || e == VFS_UNSUPPORTED ? USH_FS_ERROR : ush_fs_failure(e);
        }
        ro = true; fsname = "UltriumFS 1";
        e = vfs_mount(path, ufs_backend(), &ufs_mount_ops, fsname, d->name, true, true);
        if (e != VFS_OK) { (void)ufs_unmount(); terminal_write("Mount point must be an existing, empty, non-mounted folder of the root filesystem.\n"); return ush_fs_failure(e); }
        terminal_write("Mounted "); terminal_write(d->name); terminal_write(" (UltriumFS 1) at "); terminal_write(path);
        terminal_write(" READ-ONLY: UltriumFS 1 volumes are never modified. Copy files to an UltriumFS 2 volume to migrate.\n");
        (void)read_only;
        return USH_OK;
    }
    static struct ufs2_report report;
    e = ufs2_fs_mount(d, !read_only, &report);
    if (e == VFS_CORRUPT) {
        terminal_write("Mount refused: structural damage found; nothing was modified. Findings:\n");
        print_findings(&report, 8);
        terminal_write("Run 'ultrium storage check "); terminal_write(d->name); terminal_write("' for the full report.\n");
        return USH_FS_ERROR;
    }
    if (e == VFS_RECOVERY) {
        terminal_write("Mount refused: the journal holds committed changes that must be replayed, which needs a writable, flush-capable device. Run 'ultrium storage check "); terminal_write(d->name); terminal_write(" --repair'.\n");
        return USH_FS_ERROR;
    }
    if (e == VFS_UNSUPPORTED) { terminal_write("Mount refused: unsupported UltriumFS 2 feature or version.\n"); return USH_FS_ERROR; }
    if (e != VFS_OK) return ush_fs_failure(e);
    struct ufs2_fs_info info;
    (void)ufs2_fs_get_info(&info);
    ro = info.read_only; fsname = "UltriumFS 2";
    e = vfs_mount(path, ufs2_fs_backend(), &ufs2_mount_ops, fsname, d->name, true, ro);
    if (e != VFS_OK) {
        (void)ufs2_fs_unmount();
        terminal_write("Mount point must be an existing, empty, non-mounted folder of the root filesystem.\n");
        return ush_fs_failure(e);
    }
    terminal_write("Mounted "); terminal_write(d->name); terminal_write(" (UltriumFS 2) at "); terminal_write(path);
    terminal_write(ro ? " read-only\n" : " read-write\n");
    if (ro && !read_only && info.ro_reason) { terminal_write("Reason: "); terminal_write(info.ro_reason); terminal_write("\n"); }
    if (info.repair_recommended) { terminal_write("Repairable problems were found:\n"); print_findings(&report, 6); }
    if (info.unclean) terminal_write("Note: the previous session did not unmount cleanly; the journal was replayed and the structure verified.\n");
    if (info.orphans) { terminal_write("Note: orphaned items present (reported, not removed): "); storage_write_number(info.orphans); terminal_write("\n"); }
    return USH_OK;
}
static enum ush_result do_unmount(const char *argument) {
    char path[VFS_PATH_MAX + 1U];
    enum vfs_error e = vfs_resolve_path(argument, ush_cwd(), path, sizeof(path));
    if (e != VFS_OK) return ush_fs_failure(e);
    if (!vfs_is_mount_point(path)) { terminal_write("Not a mount point. See 'ultrium storage volumes'.\n"); return USH_FS_ERROR; }
    if (path_within(path, ush_cwd())) { terminal_write("Refusing: the current folder is inside this volume. Use 'go /' first.\n"); return USH_FS_ERROR; }
    e = vfs_unmount(path);
    if (e == VFS_OK) { terminal_write("Unmounted "); terminal_write(path); terminal_write(" (synchronized and released).\n"); return USH_OK; }
    struct vfs_mount_info info; bool still = false;
    for (size_t i = 0; vfs_mount_info_at(i, &info); ++i) if (same(info.path, path)) still = true;
    if (still) terminal_write("Unmount failed; the volume is still mounted and no changes were discarded.\n");
    else terminal_write("Unmounted with errors: the volume could not be synchronized and stays marked unclean. Remount verifies it.\n");
    return ush_fs_failure(e);
}

/* ---- sync / volumes / info ---- */
static enum ush_result do_sync(void) {
    size_t synced = 0;
    enum vfs_error e = vfs_sync_all(&synced);
    if (e != VFS_OK) { terminal_write("Synchronization FAILED; data may not be persisted.\n"); return ush_fs_failure(e); }
    if (!synced) { terminal_write("No persistent volumes are mounted.\n"); return USH_OK; }
    terminal_write("Synchronized "); storage_write_number(synced); terminal_write(" volume(s) and flushed the device.\n");
    return USH_OK;
}
static enum ush_result do_volumes(void) {
    struct vfs_mount_info m;
    for (size_t i = 0; vfs_mount_info_at(i, &m); ++i) {
        terminal_write(m.path); terminal_write("  "); terminal_write(m.filesystem);
        terminal_write(m.persistent ? "  persistent" : "  volatile");
        terminal_write("  source="); terminal_write(m.source ? m.source : "?");
        terminal_write(m.read_only ? "  read-only" : "  read-write");
        terminal_write("  open="); storage_write_number(m.open_handles); terminal_write("\n");
    }
    return USH_OK;
}
static enum ush_result do_info(const char *argument) {
    char path[VFS_PATH_MAX + 1U];
    enum vfs_error e = vfs_resolve_path(argument, ush_cwd(), path, sizeof(path));
    if (e != VFS_OK) return ush_fs_failure(e);
    struct vfs_info st;
    e = vfs_stat(path, &st);
    if (e != VFS_OK) return ush_fs_failure(e);
    terminal_write("Path: "); terminal_write(path);
    terminal_write("\nFilesystem: "); terminal_write(st.filesystem ? st.filesystem : "unknown");
    terminal_write(st.persistent ? " (persistent)\n" : " (volatile)\n");
    char base[VFS_PATH_MAX + 1U];
    struct ufs_info u; struct ufs2_fs_info u2;
    if (st.persistent && ufs_mount_path(base, sizeof(base)) && path_within(base, path) && ufs2_fs_get_info(&u2)) {
        terminal_write("Label: "); terminal_write(u2.label);
        terminal_write("\nDevice: "); terminal_write(u2.device->name);
        terminal_write("\nCapacity: "); write_size(u2.total_blocks * UFS2_BLOCK);
        terminal_write("\nFree: "); write_size(u2.free_blocks * UFS2_BLOCK);
        terminal_write("\nUsed: "); write_size((u2.total_blocks - u2.free_blocks) * UFS2_BLOCK);
        terminal_write("\nFiles/folders: "); storage_write_number(u2.inode_count - u2.free_inodes);
        terminal_write(" of "); storage_write_number(u2.inode_count);
        terminal_write("\nMode: "); terminal_write(u2.read_only ? "read-only" : "read-write");
        if (u2.read_only && u2.ro_reason) { terminal_write(" ("); terminal_write(u2.ro_reason); terminal_write(")"); }
        if (u2.faulted) terminal_write(" (FAULTED after an I/O error; unmount and remount to replay and verify)");
        terminal_write("\nJournal: write-ahead metadata journal, checkpointed per change; "); storage_write_number(u2.transactions); terminal_write(" transactions this session");
        if (u2.unclean) terminal_write("\nNote: mounted after an unclean session (journal replayed if needed)");
        if (u2.repair_recommended) terminal_write("\nRepair recommended: run 'ultrium storage check <device> --repair'");
        if (u2.orphans) { terminal_write("\nOrphaned items: "); storage_write_number(u2.orphans); }
        terminal_write("\nBlock size: 4096 B; capacity counts data blocks only.\n");
    } else if (st.persistent && ufs_mount_path(base, sizeof(base)) && path_within(base, path) && ufs_get_info(&u)) {
        terminal_write("Label: "); terminal_write(u.label);
        terminal_write("\nDevice: "); terminal_write(u.device->name);
        terminal_write("\nCapacity: "); write_size(u.total_blocks * UFS_BLOCK_SIZE);
        terminal_write("\nFree: "); write_size(u.free_blocks * UFS_BLOCK_SIZE);
        terminal_write("\nUsed: "); write_size((u.total_blocks - u.free_blocks) * UFS_BLOCK_SIZE);
        terminal_write("\nFiles/folders: "); storage_write_number(u.inode_count - u.free_inodes);
        terminal_write(" of "); storage_write_number(u.inode_count);
        terminal_write("\nMode: "); terminal_write(u.read_only ? "read-only" : "read-write");
        if (u.faulted) terminal_write(" (FAULTED after an I/O error; unmount and remount to verify)");
        if (u.unclean_mount) terminal_write("\nNote: mounted after an unclean session");
        if (u.orphan_inodes) { terminal_write("\nOrphaned items: "); storage_write_number(u.orphan_inodes); }
        terminal_write("\nBlock size: 4096 B; capacity counts data blocks only.\n");
    } else {
        terminal_write("Capacity statistics: not provided by this filesystem.\n");
    }
    return USH_OK;
}

static enum ush_result do_check(const char *device, bool repair) {
    struct block_device *d = block_find(device);
    if (!d) { terminal_write("No such block device. See 'ultrium storage devices'.\n"); return USH_FS_ERROR; }
    enum storage_format kind = storage_probe(d);
    if (kind == STORAGE_V1) { terminal_write("This is an UltriumFS 1 volume: it has no journal or checker. Mount it read-only to copy files out.\n"); return USH_FS_ERROR; }
    if (kind == STORAGE_UNKNOWN) { terminal_write("No UltriumFS volume found on this device.\n"); return USH_FS_ERROR; }
    if (d->holders || any_volume_mounted()) { terminal_write("Refusing: the device or a volume is mounted/in use. Unmount it first.\n"); return USH_FS_ERROR; }
    static struct ufs2_check_result r;
    enum vfs_error e = ufs2_check(d, repair, &r);
    if (e != VFS_OK) return ush_fs_failure(e);
    terminal_write(repair ? "UltriumFS 2 check with repair: " : "UltriumFS 2 check (read-only, nothing is modified): "); terminal_write(d->name); terminal_write("\n");
    if (r.superblock_unreadable) { terminal_write("BOTH superblock copies are damaged. This volume cannot be recovered automatically.\n"); if (r.refusal) { terminal_write("Repair refused: "); terminal_write(r.refusal); terminal_write("\n"); } return USH_FS_ERROR; }
    if (r.status == VFS_UNSUPPORTED) { terminal_write("Unsupported UltriumFS 2 version or feature flags.\n"); return USH_FS_ERROR; }
    if (r.status != VFS_OK) return ush_fs_failure(r.status);
    if (r.journal_pending) { terminal_write("Journal: "); storage_write_number(r.pending_transactions); terminal_write(" committed transaction(s) waiting for replay (the volume files below are not verified until replay)\n"); }
    terminal_write("Problems found: structural "); storage_write_number(r.before.count[2]);
    terminal_write(", repairable "); storage_write_number(r.before.count[0]); terminal_write(", orphans "); storage_write_number(r.before.count[1]); terminal_write("\n");
    print_findings(&r.before, 16);
    if (!r.journal_pending) { terminal_write("Inodes in use: "); storage_write_number(r.before.inodes_used); terminal_write(", blocks owned: "); storage_write_number(r.before.blocks_owned); terminal_write("\n"); }
    if (!repair) {
        if (r.before.count[2]) terminal_write("Writable mounting is NOT safe: the volume is damaged. No repair is attempted automatically.\n");
        else if (r.before.count[0]) terminal_write("The volume mounts read-only until repaired: run 'ultrium storage check <device> --repair'.\n");
        else terminal_write("The volume is consistent.\n");
        return r.before.count[2] ? USH_FS_ERROR : USH_OK;
    }
    if (r.repair_refused) { terminal_write("Repair REFUSED: "); terminal_write(r.refusal ? r.refusal : "unsafe"); terminal_write("\nNothing was changed.\n"); return USH_FS_ERROR; }
    if (!r.repair_ran) { terminal_write("Repair was not needed or could not start.\n"); return USH_OK; }
    terminal_write("Changes made:\n");
    for (uint32_t i = 0; i < r.action_count; ++i) { terminal_write("  "); terminal_write(r.actions[i]); terminal_write("\n"); }
    if (!r.action_count) terminal_write("  none\n");
    terminal_write("Remaining problems: structural "); storage_write_number(r.after.count[2]); terminal_write(", repairable "); storage_write_number(r.after.count[0]);
    terminal_write(", orphans "); storage_write_number(r.after.count[1]); terminal_write(" (orphans are reported, never deleted)\n");
    print_findings(&r.after, 8);
    if (r.fully_repaired) { terminal_write("Repair complete. Writable mounting is safe.\n"); return USH_OK; }
    terminal_write("Repair PARTIAL: writable mounting is not yet safe.\n");
    return USH_FS_ERROR;
}

enum ush_result storage_operation(const char *name, size_t argc, char **argv) {
    if (same(name, "sync") || same(name, "volumes")) {
        if (argc) { terminal_write("This command takes no arguments.\n"); return USH_USAGE; }
        return same(name, "sync") ? do_sync() : do_volumes();
    }
    if (same(name, "info") || same(name, "unmount")) {
        if (argc != 1) { terminal_write("Expected one path. Use '<command> help'.\n"); return USH_USAGE; }
        return same(name, "info") ? do_info(argv[0]) : do_unmount(argv[0]);
    }
    if (same(name, "format")) {
        const char *device = 0; bool yes = false;
        for (size_t i = 0; i < argc; ++i) {
            if (flag(argv[i], "-y", "--yes")) yes = true;
            else if (argv[i][0] == '-' || device) { terminal_write("Usage: ultrium storage format <device> [-y|--yes]\n"); return USH_USAGE; }
            else device = argv[i];
        }
        if (!device || text_length(device) > BLOCK_NAME_MAX) return USH_USAGE;
        struct block_device *d = block_find(device);
        if (format_precheck(d) != VFS_OK) return USH_FS_ERROR;
        if (yes) return do_format(device);
        for (size_t i = 0; i <= text_length(device); ++i) pending_device[i] = device[i];
        char question[192] = "Format '"; size_t used = text_length(question);
        for (size_t i = 0; device[i]; ++i) question[used++] = device[i];
        const char *tail = "' as UltriumFS? ALL DATA ON IT WILL BE LOST. [y/N] ";
        for (size_t i = 0; tail[i]; ++i) question[used++] = tail[i];
        question[used] = 0;
        return ush_request_confirmation(question, format_answer) ? USH_PENDING : USH_USAGE;
    }
    if (same(name, "check")) {
        const char *device = 0; bool repair = false;
        for (size_t i = 0; i < argc; ++i) {
            if (flag(argv[i], "--repair", "--repair")) repair = true;
            else if (argv[i][0] == '-' || device) { terminal_write("Usage: ultrium storage check <device> [--repair]\n"); return USH_USAGE; }
            else device = argv[i];
        }
        if (!device) { terminal_write("Usage: ultrium storage check <device> [--repair]\n"); return USH_USAGE; }
        return do_check(device, repair);
    }
    if (same(name, "mount")) {
        const char *device = 0, *path = 0; bool read_only = false;
        for (size_t i = 0; i < argc; ++i) {
            if (flag(argv[i], "-r", "--read-only")) read_only = true;
            else if (argv[i][0] == '-') { terminal_write("Usage: ultrium storage mount <device> <path> [-r|--read-only]\n"); return USH_USAGE; }
            else if (!device) device = argv[i];
            else if (!path) path = argv[i];
            else { terminal_write("Usage: ultrium storage mount <device> <path> [-r|--read-only]\n"); return USH_USAGE; }
        }
        if (!device || !path) { terminal_write("Usage: ultrium storage mount <device> <path> [-r|--read-only]\n"); return USH_USAGE; }
        return do_mount(device, path, read_only);
    }
    return USH_UNKNOWN;
}

/* ---- shutdown integration ---- */
bool storage_shutdown_sync(size_t *volumes, size_t *synced_out) {
    struct vfs_mount_info m;
    size_t count = 0;
    for (size_t i = 0; vfs_mount_info_at(i, &m); ++i) count += m.persistent;
    vfs_freeze(true);                           /* stop accepting changes */
    size_t synced = 0;
    enum vfs_error e = vfs_sync_all(&synced);   /* flushes the block devices too */
    if (volumes) *volumes = count;
    if (synced_out) *synced_out = synced;
    return e == VFS_OK;
}
void storage_shutdown_abort(void) { vfs_freeze(false); }
static void power_prepare(struct power_sync_report *report) {
    report->ok = storage_shutdown_sync(&report->volumes, &report->synced);
}
static void power_abort(void) { storage_shutdown_abort(); }

void storage_register_power_hook(void) { power_set_sync_hooks(power_prepare, power_abort); }
