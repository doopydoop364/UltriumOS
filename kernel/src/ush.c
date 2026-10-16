#include "ush.h"
#include "storage/storage_ops.h"
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
void ush_compatibility_status(void) {
    terminal_write(compatibility ? "Compatibility aliases: enabled (session only)\n" :
                                  "Compatibility aliases: disabled (native mode)\n");
}
void ush_compatibility_enable(void) { compatibility = true; ush_compatibility_status(); }
void ush_compatibility_disable(void) { compatibility = false; ush_compatibility_status(); }

static enum ush_result tokenize(const char *line, struct ush_args *args, bool partial) {
    args->count = 0; args->error = 0; args->trailing_space = false;
    size_t n = 0;
    while (line[n]) {
        if (n == USH_INPUT_MAX) { args->error = "Command exceeds 70 characters."; return USH_USAGE; }
        if ((unsigned char)line[n] < 32 || (unsigned char)line[n] > 126) {
            args->error = "Unsupported control or non-ASCII character."; return USH_USAGE;
        }
        ++n;
    }
    size_t read = 0, write = 0;
    while (read < n) {
        while (read < n && line[read] == ' ') ++read;
        if (read == n) break;
        if (args->count == USH_ARG_MAX) { args->error = "Too many arguments (maximum 16)."; return USH_USAGE; }
        args->trailing_space = false;
        args->starts[args->count] = read;
        args->values[args->count++] = args->storage + write;
        char quote = 0;
        while (read < n) {
            char c = line[read++];
            if (c == '\\') {
                if (read == n) { args->error = "Trailing escape requires a character."; return USH_USAGE; }
                args->storage[write++] = line[read++];
            } else if (quote) {
                if (c == quote) quote = 0;
                else args->storage[write++] = c;
            } else if (c == '\'' || c == '"') quote = c;
            else if (c == ' ') { args->trailing_space = true; break; }
            else args->storage[write++] = c;
        }
        if (quote && !partial) { args->error = "Unterminated quote."; return USH_USAGE; }
        args->storage[write++] = 0;
    }
    return USH_OK;
}
enum ush_result ush_tokenize(const char *line, struct ush_args *args) { return tokenize(line, args, false); }
enum ush_result ush_tokenize_prefix(const char *line, struct ush_args *args) { return tokenize(line, args, true); }
enum ush_result ush_fs_failure(enum vfs_error error);
static enum ush_result failure(enum vfs_error error) { return ush_fs_failure(error); }
enum ush_result ush_fs_failure(enum vfs_error error) {
    if (error == VFS_OK) return USH_OK;
    terminal_write("Filesystem operation failed: ");
    switch (error) {
        case VFS_NOT_FOUND: terminal_write("not found\n"); break;
        case VFS_EXISTS: terminal_write("already exists\n"); break;
        case VFS_NOT_DIRECTORY: terminal_write("not a directory\n"); break;
        case VFS_IS_DIRECTORY: terminal_write("is a directory\n"); break;
        case VFS_NO_SPACE: terminal_write("out of memory or capacity\n"); break;
        case VFS_NOT_EMPTY: terminal_write("folder is not empty; use -r to recurse\n"); break;
        case VFS_INVALID_PATH: terminal_write("invalid path\n"); break;
        case VFS_READ_ONLY: terminal_write("read-only volume\n"); break;
        case VFS_IO: terminal_write("storage I/O error\n"); break;
        case VFS_CORRUPT: terminal_write("on-disk structure is corrupt\n"); break;
        case VFS_BUSY: terminal_write("busy (open files, mount point, or shutting down)\n"); break;
        case VFS_DENIED: terminal_write("not permitted\n"); break;
        case VFS_RECOVERY: terminal_write("recovery required (mount read-write on a writable device, or run 'ultrium storage check')\n"); break;
        default: terminal_write("unsupported or busy operation\n"); break;
    }
    return USH_FS_ERROR;
}
static void number(size_t value) {
    char digits[21]; size_t n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) terminal_putc_color(digits[--n], TERMINAL_NORMAL);
}
static char confirmation[192];
static void (*confirmation_answer)(bool);
bool ush_request_confirmation(const char *question, void (*answer)(bool)) {
    if (!question || !answer || confirmation_answer) return false;
    size_t n = length(question);
    if (n >= sizeof(confirmation)) return false;
    for (size_t i = 0; i <= n; ++i) confirmation[i] = question[i];
    confirmation_answer = answer;
    return true;
}
bool ush_confirmation_pending(void) { return confirmation_answer != 0; }
const char *ush_confirmation_prompt(void) { return confirmation; }
enum ush_result ush_answer_confirmation(const char *line) {
    if (!confirmation_answer) return USH_USAGE;
    void (*answer)(bool) = confirmation_answer;
    confirmation_answer = 0;
    answer(ush_word_equal(line, "y") || ush_word_equal(line, "yes"));
    return USH_OK;
}
static bool protected_path(const char *path) {
    size_t n = length(path);
    return n == 1 || (length(cwd) >= n && __builtin_memcmp(path, cwd, n) == 0 &&
                    (cwd[n] == '/' || cwd[n] == 0));
}
static char removal_path[VFS_PATH_MAX + 1U];
static void report_removed(const char *path) {
    terminal_write("Removed: "); terminal_write(path); terminal_write("\n");
}
static enum ush_result remove_tree(const char *path) {
    struct vfs_remove_report report;
    enum vfs_error error = vfs_remove_tree_observe(path, &report, report_removed);
    terminal_write("Removed items: "); number(report.removed); terminal_write("\n");
    if (error != VFS_OK) {
        terminal_write("Removal stopped. Remaining tree: "); terminal_write(path);
        terminal_write("\nBlocked at: "); terminal_write(report.remaining); terminal_write("\n");
    }
    return failure(error);
}
static void removal_answer(bool yes) {
    if (yes) (void)remove_tree(removal_path);
    else terminal_write("Removal cancelled.\n");
}
static enum ush_result remove_item(size_t argc, char **argv) {
    bool recursive = false, yes = false;
    const char *argument = 0;
    for (size_t i = 0; i < argc; ++i) {
        if (ush_word_equal(argv[i], "-r") || ush_word_equal(argv[i], "--recursive")) recursive = true;
        else if (ush_word_equal(argv[i], "-y") || ush_word_equal(argv[i], "--yes")) yes = true;
        else if (argv[i][0] == '-' || argument) {
            terminal_write("Unsupported remove option or extra path.\n"); return USH_USAGE;
        } else argument = argv[i];
    }
    if (!argument) return USH_USAGE;
    char path[VFS_PATH_MAX + 1U];
    enum vfs_error error = vfs_resolve_path(argument, cwd, path, sizeof(path));
    if (error != VFS_OK) return failure(error);
    if (protected_path(path)) {
        terminal_write("Removal refused: root, current directory, or ancestor.\n"); return USH_FS_ERROR;
    }
    struct vfs_info info;
    error = vfs_stat(path, &info);
    if (error != VFS_OK) return failure(error);
    if (!recursive || info.type == VFS_NODE_FILE) return failure(vfs_unlink(path));
    if (yes) return remove_tree(path);
    for (size_t i = 0; i <= length(path); ++i) removal_path[i] = path[i];
    char question[192]; size_t used = 0;
    const char *parts[] = {"Remove '", path, "' and all its contents? [y/N] "};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; parts[i][j]; ++j) question[used++] = parts[i][j];
    question[used] = 0;
    return ush_request_confirmation(question, removal_answer) ? USH_PENDING : USH_USAGE;
}
enum ush_result ush_operation(const char *name, size_t argc, char **argv) {
    if (name[0] == 's' && name[1] == 't' && name[2] == 'o' && name[3] == 'r' && name[4] == 'a' && name[5] == 'g' && name[6] == 'e' && name[7] == ' ')
        return storage_operation(name + 8, argc, argv);
    if (ush_word_equal(name, "remove")) return remove_item(argc, argv);
    bool transfer = ush_word_equal(name, "copy") || ush_word_equal(name, "move") || ush_word_equal(name, "rename");
    bool list = ush_word_equal(name, "list"), where = ush_word_equal(name, "where");
    bool write = ush_word_equal(name, "write");
    if ((where && argc) || (list && argc > 1) ||
        (!where && !list && !write && !transfer && argc != 1) || (write && argc < 2) || (transfer && argc != 2)) {
        terminal_write("Invalid arguments. Use '<command> help'.\n"); return USH_USAGE;
    }
    if (where) { terminal_write(cwd); terminal_write("\n"); return USH_OK; }
    char path[VFS_PATH_MAX + 1U];
    enum vfs_error error = vfs_resolve_path(argc ? argv[0] : ".", cwd, path, sizeof(path));
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
    if (transfer) {
        char destination[VFS_PATH_MAX + 1U];
        error = vfs_resolve_path(argv[1], cwd, destination, sizeof(destination));
        if (error != VFS_OK) return failure(error);
        if (!ush_word_equal(name, "copy") && protected_path(path)) {
            terminal_write("Move refused: root, current directory, or ancestor.\n"); return USH_FS_ERROR;
        }
        return failure(ush_word_equal(name, "copy") ? vfs_copy(path, destination) : vfs_rename(path, destination));
    }
    if (ush_word_equal(name, "info")) {
        struct vfs_info info;
        error = vfs_stat(path, &info);
        if (error != VFS_OK) return failure(error);
        terminal_write("Path: "); terminal_write(path);
        terminal_write(info.type == VFS_NODE_DIRECTORY ? "\nType: folder\n" : "\nType: file\nBytes: ");
        if (info.type == VFS_NODE_FILE) { number(info.size); terminal_write("\n"); }
        terminal_write("Storage: "); terminal_write(info.filesystem ? info.filesystem : "unknown");
        terminal_write(info.persistent ? " (persistent)\n" : " (volatile)\n"); return USH_OK;
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
