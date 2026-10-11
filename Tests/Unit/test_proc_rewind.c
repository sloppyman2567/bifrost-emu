// Synthetic files must rewind like Linux procfs/sysfs. Unity keeps cpuinfo
// open and rereads it when refreshing its job-worker topology.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *path, const char *what) {
    printf("%s %s: %s\n", ok ? "PASS" : "FAIL", path, what);
    if (!ok) ++failures;
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
    rewind_file("/proc/cpuinfo");
    rewind_file("/sys/devices/system/cpu/online");
    rewind_file("/proc/self/maps");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    puts("ALL PASS");
    return 0;
}
