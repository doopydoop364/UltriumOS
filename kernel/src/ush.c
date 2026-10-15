#include "ush.h"
#include "terminal/terminal.h"

static char cwd[VFS_PATH_MAX + 1U] = "/";
static bool compatibility;
static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
bool ush_word_equal(const char *a, const char *b) {
    for (;;) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
        if (!x) return true;
    }
}
const char *ush_cwd(void) { return cwd; }
bool ush_compatibility(void) { return compatibility; }
void ush_version(void) { terminal_write("Ultrium Shell (USH) " USH_VERSION "\n"); }
void ush_help(void) {
    ush_version();
    terminal_write("Simple actions should be short. Complex operations should be structured.\nEverything should be discoverable.\n");
    terminal_write("Native: list, go, where, new file, new folder, view, write, remove, info\n");
    terminal_write("ultrium shell version\nultrium shell compatibility help\n");
}
void ush_compatibility_help(void) {
    terminal_write("ultrium shell compatibility status|enable|disable\nSession only. Selected aliases; no POSIX or Bash compatibility.\n");
}
void ush_compatibility_status(void) {
    terminal_write(compatibility ? "Compatibility aliases: enabled (session only)\n" :
                                  "Compatibility aliases: disabled (native mode)\n");
}
void ush_compatibility_enable(void) { compatibility = true; ush_compatibility_status(); }
void ush_compatibility_disable(void) { compatibility = false; ush_compatibility_status(); }

enum ush_result ush_tokenize(const char *line, struct ush_args *args) {
    args->count = 0;
    size_t n = 0;
    while (line[n]) {
        if (n == USH_INPUT_MAX) return USH_USAGE;
        args->storage[n] = line[n]; ++n;
    }
    args->storage[n] = 0;
    char *p = args->storage;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (args->count == USH_ARG_MAX) return USH_USAGE;
        args->values[args->count++] = p;
        while (*p && *p != ' ') ++p;
        if (*p) *p++ = 0;
    }
    return USH_OK;
}
static enum ush_result failure(enum vfs_error error) {
    if (error == VFS_OK) return USH_OK;
    terminal_write("Filesystem operation failed: ");
    switch (error) {
        case VFS_NOT_FOUND: terminal_write("not found\n"); break;
        case VFS_EXISTS: terminal_write("already exists\n"); break;
        case VFS_NOT_DIRECTORY: terminal_write("not a directory\n"); break;
        case VFS_IS_DIRECTORY: terminal_write("is a directory\n"); break;
        case VFS_NO_SPACE: terminal_write("out of memory or capacity\n"); break;
        case VFS_INVALID_PATH: terminal_write("invalid path\n"); break;
        default: terminal_write("unsupported or busy operation\n"); break;
    }
    return USH_FS_ERROR;
}
static void number(size_t value) {
    char digits[21]; size_t n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) terminal_putc_color(digits[--n], TERMINAL_NORMAL);
}
enum ush_result ush_operation(const char *name, size_t argc, char **argv) {
    bool list = ush_word_equal(name, "list"), where = ush_word_equal(name, "where");
    bool write = ush_word_equal(name, "write");
    if ((where && argc) || (list && argc > 1) ||
        (!where && !list && !write && argc != 1) || (write && argc < 2)) {
        terminal_write("Invalid arguments. Use '<command> help'.\n"); return USH_USAGE;
    }
    if (where) { terminal_write(cwd); terminal_write("\n"); return USH_OK; }
    char path[VFS_PATH_MAX + 1U];
    enum vfs_error error = vfs_normalize(argc ? argv[0] : ".", cwd, path, sizeof(path));
    if (error != VFS_OK) return failure(error);
    if (list) {
        struct vfs_dir dir = {0};
        error = vfs_dir_open(path, &dir);
        if (error != VFS_OK) return failure(error);
        char entry[VFS_NAME_MAX + 1U]; enum vfs_node_type type;
        while ((error = vfs_dir_next(&dir, entry, sizeof(entry), &type)) == VFS_OK) {
            terminal_write(entry); if (type == VFS_NODE_DIRECTORY) terminal_write("/");
            terminal_write("\n");
        }
        enum vfs_error closed = vfs_dir_close(&dir);
        return failure(error == VFS_END ? closed : error);
    }
    if (ush_word_equal(name, "go")) {
        struct vfs_info info;
        error = vfs_stat(path, &info);
        if (error != VFS_OK) return failure(error);
        if (info.type != VFS_NODE_DIRECTORY) return failure(VFS_NOT_DIRECTORY);
        for (size_t i = 0; i <= length(path); ++i) cwd[i] = path[i];
        return USH_OK;
    }
    if (ush_word_equal(name, "new folder")) return failure(vfs_create_directory(path));
    if (ush_word_equal(name, "new file")) return failure(vfs_create_file(path));
    if (ush_word_equal(name, "remove")) {
        size_t n = length(path);
        if (n == 1 || (length(cwd) >= n && __builtin_memcmp(path, cwd, n) == 0 &&
            (cwd[n] == '/' || cwd[n] == 0))) {
            terminal_write("Removal refused: root, current directory, or ancestor.\n");
            return USH_FS_ERROR;
        }
        return failure(vfs_unlink(path));
    }
    if (ush_word_equal(name, "info")) {
        struct vfs_info info;
        error = vfs_stat(path, &info);
        if (error != VFS_OK) return failure(error);
        terminal_write("Path: "); terminal_write(path);
        terminal_write(info.type == VFS_NODE_DIRECTORY ? "\nType: folder\n" : "\nType: file\nBytes: ");
        if (info.type == VFS_NODE_FILE) { number(info.size); terminal_write("\n"); }
        terminal_write("Storage: volatile RAMFS\n"); return USH_OK;
    }
    if (write) {
        char contents[USH_INPUT_MAX + 1U]; size_t used = 0;
        for (size_t i = 1; i < argc; ++i) {
            if (i > 1) contents[used++] = ' ';
            for (size_t j = 0; argv[i][j]; ++j) contents[used++] = argv[i][j];
        }
        error = vfs_create_file(path);
        if (error != VFS_OK) return failure(error);
        struct vfs_file file = {0}; size_t written = 0;
        error = vfs_file_open(path, VFS_OPEN_WRITE, &file);
        if (error == VFS_OK) error = vfs_file_write(&file, contents, used, &written);
        if (file.generation) {
            enum vfs_error closed = vfs_file_close(&file);
            if (closed != VFS_OK) error = closed;
        }
        if (error != VFS_OK || written != used) {
            enum vfs_error rollback = vfs_unlink(path);
            if (rollback != VFS_OK) terminal_write("Write rollback failed; destination remains.\n");
            return failure(error == VFS_OK ? VFS_NO_SPACE : error);
        }
        return USH_OK;
    }
    if (ush_word_equal(name, "view")) {
        struct vfs_file file = {0};
        error = vfs_file_open(path, VFS_OPEN_READ, &file);
        if (error != VFS_OK) return failure(error);
        char buffer[128]; size_t count;
        while ((error = vfs_file_read(&file, buffer, sizeof(buffer), &count)) == VFS_OK && count)
            for (size_t i = 0; i < count; ++i) terminal_putc_color(buffer[i], TERMINAL_NORMAL);
        enum vfs_error closed = vfs_file_close(&file);
        terminal_write("\n");
        return failure(error == VFS_OK ? closed : error);
    }
    return USH_UNKNOWN;
}
