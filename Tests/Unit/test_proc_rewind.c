// Synthetic files must rewind like Linux procfs/sysfs. Unity keeps cpuinfo
// open and rereads it when refreshing its job-worker topology.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *path, const char *what) {
    printf("%s %s: %s\n", ok ? "PASS" : "FAIL", path, what);
    if (!ok) ++failures;
}

static void proc_identity(const char *directory, const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    DIR *dir = opendir(directory);
    int fd = open(path, O_RDONLY), second = open(path, O_RDONLY);
    check(dir && fd >= 0 && second >= 0, path, "identity opens");
    if (!dir || fd < 0 || second < 0) goto done;
    struct stat a = {0}, b = {0}, c = {0};
    check(stat(path, &a) == 0 && fstat(fd, &b) == 0 && fstat(second, &c) == 0 &&
          a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
          a.st_dev == c.st_dev && a.st_ino == c.st_ino,
          path, "path stat and independent descriptors share identity");
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, name)) continue;
        found = 1;
        check(entry->d_ino == a.st_ino, path, "directory inode matches stat");
        break;
    }
    check(found, path, "listed in parent");
    check(fstatat(dirfd(dir), name, &c, 0) == 0 &&
          a.st_dev == c.st_dev && a.st_ino == c.st_ino,
          path, "relative dirfd stat matches");
    check(fstatat(fd, "", &c, AT_EMPTY_PATH) == 0 &&
          a.st_dev == c.st_dev && a.st_ino == c.st_ino,
          path, "empty path stat matches descriptor");
    char slash[260];
    snprintf(slash, sizeof(slash), "%s/", path);
    errno = 0;
    check(stat(slash, &c) == -1 && errno == ENOTDIR,
          path, "trailing slash requires a directory");
    errno = 0;
    check(fstatat(AT_FDCWD, path, &c, 0x40000000) == -1 && errno == EINVAL,
          path, "unknown stat flags rejected");
#ifdef SYS_statx
    uint8_t sx[256] = {0};
    uint64_t ino;
    long result = syscall(SYS_statx, AT_FDCWD, path, 0, 0x100 /* STATX_INO */, sx);
    memcpy(&ino, sx + 32, sizeof(ino));
    check(result == 0 && ino == a.st_ino, path, "statx inode matches");
    memset(sx, 0, sizeof(sx));
    result = syscall(SYS_statx, fd, "", AT_EMPTY_PATH, 0x100, sx);
    memcpy(&ino, sx + 32, sizeof(ino));
    check(result == 0 && ino == a.st_ino, path, "empty path statx inode matches");
#endif
done:
    if (dir) closedir(dir);
    if (fd >= 0) close(fd);
    if (second >= 0) close(second);
}

static void rewind_file(const char *path) {
    int fd = open(path, O_RDONLY);
    check(fd >= 0, path, "open");
    if (fd < 0) return;
    char first[128], again[128], discard[4096];
    ssize_t n = read(fd, first, sizeof first);
    check(n > 0, path, "initial content");
    if (n <= 0) { close(fd); return; }
    check(lseek(fd, 0, SEEK_SET) == 0 && lseek(fd, 0, SEEK_CUR) == 0,
          path, "rewind resets actual offset");
    ssize_t m = read(fd, again, sizeof again);
    check(m == n && memcmp(first, again, n) == 0, path, "prefix reread");
    while (read(fd, discard, sizeof discard) > 0) {}
    check(lseek(fd, 0, SEEK_SET) == 0, path, "rewind after EOF");
    m = read(fd, again, sizeof again);
    check(m == n && memcmp(first, again, n) == 0, path, "content after EOF");
    close(fd);
}

int main(void) {
    proc_identity("/proc", "filesystems");
    proc_identity("/proc", "cpuinfo");
    proc_identity("/proc/self", "maps");
    char own[64];
    snprintf(own, sizeof(own), "/proc/%d", getpid());
    proc_identity(own, "environ");
    rewind_file("/proc/cpuinfo");
    rewind_file("/sys/devices/system/cpu/online");
    rewind_file("/proc/self/maps");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    puts("ALL PASS");
    return 0;
}
