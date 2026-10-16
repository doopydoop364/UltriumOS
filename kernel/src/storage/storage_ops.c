#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "storage_ops.h"
#include "storage_cmd.h"
#include "block.h"
#include "fs/vfs.h"
#include "fs/ultriumfs.h"
#include "system/power.h"
#include "terminal/terminal.h"

static const struct vfs_mount_ops ufs_mount_ops = {ufs_sync, ufs_unmount, ufs_mounted};

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
        if (same(info.filesystem, "UltriumFS")) {
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
    if (d->holders || ufs_mounted()) { terminal_write("Refusing: the device or a volume is mounted/in use. Unmount it first.\n"); return VFS_BUSY; }
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
    terminal_write("Formatting "); terminal_write(name); terminal_write(" as UltriumFS...\n");
    e = ufs_format(d, "ULTRIUM");
    if (e != VFS_OK) return ush_fs_failure(e);
    terminal_write("Formatted. Mount it with 'ultrium storage mount "); terminal_write(name); terminal_write(" <folder>'.\n");
    return USH_OK;
}
static void format_answer(bool yes) {
    if (yes) (void)do_format(pending_device); else terminal_write("Format cancelled.\n");
}

/* ---- mount / unmount ---- */
static enum ush_result do_mount(const char *device, const char *argument, bool read_only) {
    struct block_device *d = block_find(device);
    if (!d) { terminal_write("No such block device. See 'ultrium storage devices'.\n"); return USH_FS_ERROR; }
    char path[VFS_PATH_MAX + 1U];
    enum vfs_error e = vfs_resolve_path(argument, ush_cwd(), path, sizeof(path));
    if (e != VFS_OK) return ush_fs_failure(e);
    if (ufs_mounted()) { terminal_write("An UltriumFS volume is already mounted (one at a time).\n"); return USH_FS_ERROR; }
    e = ufs_mount(d, read_only);
    if (e == VFS_CORRUPT || e == VFS_UNSUPPORTED) {
        terminal_write(e == VFS_CORRUPT ? "Mount refused: no valid UltriumFS here, or the structure failed verification.\n"
                                       : "Mount refused: unsupported UltriumFS version or feature.\n");
        terminal_write("Nothing was modified. Format scratch disks with 'ultrium storage format'.\n");
        return USH_FS_ERROR;
    }
    if (e != VFS_OK) return ush_fs_failure(e);
    struct ufs_info info;
    (void)ufs_get_info(&info);
    e = vfs_mount(path, ufs_backend(), &ufs_mount_ops, "UltriumFS", d->name, true, info.read_only);
    if (e != VFS_OK) {
        (void)ufs_unmount();
        terminal_write("Mount point must be an existing, empty, non-mounted folder of the root filesystem.\n");
        return ush_fs_failure(e);
    }
    terminal_write("Mounted "); terminal_write(d->name); terminal_write(" (UltriumFS) at "); terminal_write(path);
    terminal_write(info.read_only ? " read-only\n" : " read-write\n");
    if (info.unclean_mount) terminal_write("Note: the volume was not cleanly synchronized last time; structure verified, recent changes may be missing.\n");
    if (info.orphan_inodes) { terminal_write("Note: orphaned items present: "); storage_write_number(info.orphan_inodes); terminal_write("\n"); }
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
    struct ufs_info u;
    if (st.persistent && ufs_mount_path(base, sizeof(base)) && path_within(base, path) && ufs_get_info(&u)) {
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
