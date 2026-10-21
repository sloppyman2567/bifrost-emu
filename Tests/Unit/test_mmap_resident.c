// A large anonymous mapping at a hinted address (ART's 512 MiB heap) must not be committed by the
// mapping itself, and reused address space must still read as zero, whatever the size.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static long rss_pages(void) {
    long size = 0, resident = -1;
    FILE *statm = fopen("/proc/self/statm", "r");
    if (statm && fscanf(statm, "%ld %ld", &size, &resident) != 2) resident = -1;
    if (statm) fclose(statm);
    return resident;
}

// Fill, unmap, map the same range again: every byte must be zero.
static void check_reuse_zero(size_t len) {
    char *first = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(first != MAP_FAILED);
    if (first == MAP_FAILED) return;
    long page = sysconf(_SC_PAGESIZE);
    for (size_t at = 0; at < len; at += page) first[at] = (char)0xa5;
    munmap(first, len);
    char *again = mmap(first, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(again == first);
    int dirty = 0;
    for (size_t at = 0; again != MAP_FAILED && at < len; at += page) dirty += again[at] != 0;
    CHECK(dirty == 0);
    munmap(again, len);
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    const size_t len = 512u << 20;
    // A hint makes the allocator zero the range it hands out, as it does for a reused one.
    long before = rss_pages();
    char *heap = mmap((void *)0x14000000, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(heap != MAP_FAILED);
    if (heap != MAP_FAILED) {
        for (size_t at = 0; at < len; at += 64u << 20) heap[at] = 1;
        long after = rss_pages();
        CHECK(before >= 0 && after >= 0 && (after - before) * page < (64l << 20));
        CHECK(heap[len - 1] == 0 && heap[len / 2 + 5] == 0);
        munmap(heap, len);
    }
    check_reuse_zero(1u << 20);    // zeroed eagerly
    check_reuse_zero(32u << 20);   // zeroed lazily
    printf(failures ? "test_mmap_resident: %d of %d checks FAILED\n" : "test_mmap_resident: ALL PASS (%d checks)\n",
           failures ? failures : checks, checks);
    return failures != 0;
}
