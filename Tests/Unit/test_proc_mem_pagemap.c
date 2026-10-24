// test_proc_mem_pagemap.c — /proc/self/mem and /proc/self/pagemap describe this process's own
// address space.
//
// A process may open its own mem and read or write memory by address; an unmapped address fails
// with EIO. pagemap has one 64-bit entry per page: bit 63 present, bit 61 file-backed or
// shared-anon, bit 56 exclusively mapped; the frame number stays 0 without CAP_SYS_ADMIN.
// Anti-tamper code (Umamusume's CrackProof) opens both before it decides whether to continue; an
// ENOENT from /proc/<pid>/mem, or pagemap entries of the host's address space, are not what a
// Linux process sees.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s (errno %d)\n", what, errno); failures++; } } while (0)

static volatile int global_value = 0x1234abcd;

static uint64_t pagemap_entry(int fd, const void *address) {
    uint64_t entry = 0;
    off_t offset = (off_t)((uintptr_t)address / 4096) * 8;
    if (pread(fd, &entry, sizeof entry, offset) != sizeof entry) return ~0ull;
    return entry;
}

static void check_mem(const char *path) {
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0, path);
    if (fd < 0) return;
    int read_back = 0;
    CHECK(pread(fd, &read_back, sizeof read_back, (off_t)(uintptr_t)&global_value) == sizeof read_back, "read own memory");
    CHECK(read_back == 0x1234abcd, "read value");
    int replacement = 0x5eed0042;
    CHECK(pwrite(fd, &replacement, sizeof replacement, (off_t)(uintptr_t)&global_value) == sizeof replacement, "write own memory");
    CHECK(global_value == 0x5eed0042, "the write is visible to the program");
    global_value = 0x1234abcd;
    // An address that was mapped and then released is unmapped.
    void *gone = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(gone != MAP_FAILED, "mmap");
    munmap(gone, 4096);
    char byte;
    errno = 0;
    CHECK(pread(fd, &byte, 1, (off_t)(uintptr_t)gone) == -1 && errno == EIO, "an unmapped address fails with EIO");
    close(fd);
}

int main(void) {
    char path[64];
    check_mem("/proc/self/mem");
    snprintf(path, sizeof path, "/proc/%d/mem", getpid());
    check_mem(path);

    int fd = open("/proc/self/pagemap", O_RDONLY);
    CHECK(fd >= 0, "open /proc/self/pagemap");
    if (fd >= 0) {
        void *anon = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(anon != MAP_FAILED, "mmap");
        *(volatile char *)anon = 1;  // touch it, so it is present on a real kernel too
        uint64_t code = pagemap_entry(fd, (void *)main), data = pagemap_entry(fd, anon);
        CHECK(code != ~0ull && (code >> 63) == 1, "a code page is present");
        CHECK((code >> 61 & 1) == 1, "a code page is file-backed");
        CHECK(data != ~0ull && (data >> 63) == 1, "an anonymous page is present");
        CHECK((data >> 61 & 1) == 0, "an anonymous page is not file-backed");
        CHECK((code & ((1ull << 55) - 1)) == 0 && (data & ((1ull << 55) - 1)) == 0, "the page frame number is hidden");
        munmap(anon, 4096);
        CHECK(pagemap_entry(fd, anon) == 0, "an unmapped page has an empty entry");
        close(fd);
    }
    CHECK(open("/proc/self/pagemap", O_WRONLY) == -1 && (errno == EACCES || errno == EPERM || errno == EINVAL), "pagemap is read-only");
    printf(failures ? "test_proc_mem_pagemap: FAILED\n" : "test_proc_mem_pagemap: ALL PASS\n");
    return failures != 0;
}
