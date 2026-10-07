// test_dev_urandom.c — /dev/urandom and /dev/random are character
// devices that never run dry (drivers/char/random.c): every read returns
// fresh bytes, an open stream can be read forever, lseek is a no-op that
// returns 0, and writes are accepted.
//
// Teeworlds opens /dev/urandom once (secure_random_init) and traps on any
// short fread in secure_random_fill. The old devfs node was a memfd
// seeded with 256 bytes, so the game died minutes in once that ran out.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void check_chrdev(const char* path, unsigned minor_num) {
    char what[96];
    struct stat st;
    int ok = stat(path, &st) == 0 && S_ISCHR(st.st_mode) &&
             major(st.st_rdev) == 1 && minor(st.st_rdev) == minor_num;
    snprintf(what, sizeof what, "%s is char device 1:%u", path, minor_num);
    check(ok, what);
}

int main(void) {
    check_chrdev("/dev/urandom", 9);
    check_chrdev("/dev/random", 8);

    // Teeworlds' pattern: one stdio stream, many small reads.
    FILE* f = fopen("/dev/urandom", "rb");
    check(f != NULL, "fopen /dev/urandom");
    if (!f) return 1;
    unsigned char a[16], b[16];
    int full = 1;
    for (int i = 0; i < 4096 && full; ++i)
        full = fread(a, 1, sizeof a, f) == sizeof a;
    check(full, "4096 x 16-byte fread on one stream");
    check(fread(b, 1, sizeof b, f) == sizeof b && memcmp(a, b, sizeof a) != 0,
          "consecutive reads differ");
    fclose(f);

    int fd = open("/dev/urandom", O_RDWR | O_CLOEXEC);
    check(fd >= 0, "open /dev/urandom O_RDWR");
    if (fd < 0) return 1;
    size_t big = 1u << 20;
    unsigned char* buf = calloc(1, big);
    check(buf && read(fd, buf, big) == (ssize_t)big, "1 MiB read is complete");
    size_t zeros = 0;
    for (size_t i = 0; buf && i < big; ++i) zeros += buf[i] == 0;
    // Expected 4096; a zero-filled tail would push this near 1M.
    check(buf && zeros < 8192, "1 MiB read is not zero-filled");
    free(buf);
    check(lseek(fd, 0, SEEK_SET) == 0 && lseek(fd, 100, SEEK_SET) == 0,
          "lseek is a no-op returning 0");
    check(write(fd, "entropy", 7) == 7, "write is accepted");
    check(read(fd, a, sizeof a) == (ssize_t)sizeof a, "read after write");
    close(fd);

    // /dev/random never blocks once the CRNG is ready (Linux 5.6+);
    // O_NONBLOCK keeps the test bounded on older kernels.
    fd = open("/dev/random", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, a, sizeof a) : -1;
    check(r == (ssize_t)sizeof a || (r < 0 && errno == EAGAIN),
          "/dev/random read");
    if (fd >= 0) close(fd);

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
