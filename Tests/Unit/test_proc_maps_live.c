// test_proc_maps_live.c — /proc/self/maps follows every change to the address space.
//
// The emulator builds the list once per change of the guest's mappings and hands the same list (and
// the same text) to every read until the next change. A stale answer would show here: an anonymous
// mapping, an mprotect, a file mapping, an munmap and a brk growth must each be visible on the very
// next read, and a read with no change in between must return the same text.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PAGE 4096u
#define TEXT_MAX (8u << 20)

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s (errno %d)\n", what, errno); failures++; } } while (0)

static char text[TEXT_MAX];

// Reads /proc/self/maps into `text` with plain read(2): stdio would allocate and move the heap.
static size_t read_maps(void) {
    int fd = open("/proc/self/maps", O_RDONLY);
    size_t used = 0;
    if (fd < 0) return 0;
    for (;;) {
        ssize_t n = read(fd, text + used, TEXT_MAX - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    text[used] = '\0';
    return used;
}

// The line that covers `address`: copies its permissions to `perms` and returns it, or NULL.
static const char *line_covering(uintptr_t address, char perms[5]) {
    for (const char *line = text; *line;) {
        unsigned long long lo, hi;
        char p[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, p) == 3 && address >= lo && address < hi) {
            memcpy(perms, p, 4);
            perms[4] = '\0';
            return line;
        }
        const char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    return NULL;
}

static int page_is(uintptr_t address, const char *want) {
    char perms[5];
    read_maps();
    return line_covering(address, perms) && !strcmp(perms, want);
}

static int page_is_unmapped(uintptr_t address) {
    char perms[5];
    read_maps();
    return line_covering(address, perms) == NULL;
}

static void unchanged_reads(void) {
    static char first[TEXT_MAX];
    size_t a = read_maps();
    memcpy(first, text, a + 1);
    int same = 1;
    for (int i = 0; i < 50; ++i) {
        size_t b = read_maps();
        if (b != a || memcmp(first, text, a)) same = 0;
    }
    CHECK(a > 0, "maps is not empty");
    CHECK(same, "reads with no mapping change return the same text");
}

static void anonymous_mapping(void) {
    uint8_t *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "mmap anonymous");
    if (p == MAP_FAILED) return;
    uintptr_t a = (uintptr_t)p;
    CHECK(page_is(a, "rw-p") && page_is(a + PAGE, "rw-p") && page_is(a + 2 * PAGE, "rw-p"),
          "new anonymous mapping is listed rw-p");
    CHECK(mprotect(p + PAGE, PAGE, PROT_READ) == 0, "mprotect middle page");
    CHECK(page_is(a, "rw-p") && page_is(a + PAGE, "r--p") && page_is(a + 2 * PAGE, "rw-p"),
          "mprotect splits the line");
    CHECK(mprotect(p + PAGE, PAGE, PROT_READ | PROT_WRITE) == 0, "mprotect back");
    CHECK(page_is(a, "rw-p") && page_is(a + PAGE, "rw-p") && page_is(a + 2 * PAGE, "rw-p"),
          "mprotect back is listed");
    CHECK(munmap(p + PAGE, PAGE) == 0, "munmap middle page");
    CHECK(page_is(a, "rw-p") && page_is_unmapped(a + PAGE) && page_is(a + 2 * PAGE, "rw-p"),
          "a hole shows after munmap of one page");
    CHECK(munmap(p, PAGE) == 0 && munmap(p + 2 * PAGE, PAGE) == 0, "munmap rest");
    CHECK(page_is_unmapped(a) && page_is_unmapped(a + 2 * PAGE), "unmapped pages are gone");
}

static void replaced_mapping(void) {
    uint8_t *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "mmap for replacement");
    if (p == MAP_FAILED) return;
    uintptr_t a = (uintptr_t)p;
    CHECK(page_is(a, "rw-p"), "mapping listed before replacement");
    void *q = mmap(p, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(q == p, "MAP_FIXED replacement");
    CHECK(page_is(a, "r--p") && page_is(a + PAGE, "rw-p"), "MAP_FIXED replacement is listed");
    munmap(p, 2 * PAGE);
}

static void file_mapping(void) {
    char path[] = "/tmp/maps-live-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd < 0) return;
    unlink(path);  // the name stays in maps with " (deleted)" on Linux; match the unique suffix only
    CHECK(ftruncate(fd, 2 * PAGE) == 0, "ftruncate");
    uint8_t *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "mmap file");
    if (p != MAP_FAILED) {
        char perms[5];
        read_maps();
        const char *line = line_covering((uintptr_t)p, perms);
        const char *end = line ? strchr(line, '\n') : NULL;
        char needle[32];
        snprintf(needle, sizeof needle, "%s", strrchr(path, '/') + 1);
        int named = 0;
        for (const char *at = line; end && at && at + strlen(needle) <= end; ++at)
            if (!strncmp(at, needle, strlen(needle))) { named = 1; break; }
        CHECK(line && !strcmp(perms, "rw-s"), "file mapping is listed rw-s");
        CHECK(named, "file mapping line names the file");
        munmap(p, 2 * PAGE);
        CHECK(page_is_unmapped((uintptr_t)p), "file mapping is gone after munmap");
    }
    close(fd);
}

static void heap_growth(void) {
    uintptr_t before = (uintptr_t)syscall(SYS_brk, 0);
    uintptr_t top = (before + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    uintptr_t grown = (uintptr_t)syscall(SYS_brk, top + 16 * PAGE);
    CHECK(grown == top + 16 * PAGE, "brk grows by 16 pages");
    if (grown != top + 16 * PAGE) return;
    char perms[5];
    read_maps();
    const char *line = line_covering(grown - 1, perms);
    const char *end = line ? strchr(line, '\n') : NULL;
    int heap = 0;
    for (const char *at = line; end && at && at + 6 <= end; ++at)
        if (!strncmp(at, "[heap]", 6)) { heap = 1; break; }
    CHECK(line && heap, "the last page of the grown break is in [heap]");
    CHECK((uintptr_t)syscall(SYS_brk, top) == top, "brk shrinks back");
    CHECK(page_is_unmapped(grown - 1), "the released break pages are gone from maps");
}

// Many changes in a row: each map/unmap must show on the next read.
static void churn(void) {
    int bad = 0;
    for (int i = 0; i < 300; ++i) {
        uint8_t *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { bad = 1; break; }
        if (!page_is((uintptr_t)p, "rw-p")) bad = 1;
        munmap(p, PAGE);
        if (!page_is_unmapped((uintptr_t)p)) bad = 1;
    }
    CHECK(!bad, "300 map/unmap pairs are each visible on the next read");
}

// Two threads read maps in a loop while this one forks: a child must still read maps and exit, whichever
// lock of the cache a reader held at the moment of the fork.
static volatile int readers_stop;
static void *reader(void *unused) {
    (void)unused;
    static char local[1 << 20];
    while (!readers_stop) {
        int fd = open("/proc/self/maps", O_RDONLY);
        if (fd < 0) continue;
        while (read(fd, local, sizeof local) > 0) {}
        close(fd);
    }
    return NULL;
}

static void fork_while_reading(void) {
    pthread_t threads[2];
    int started = 0;
    for (; started < 2; ++started)
        if (pthread_create(&threads[started], NULL, reader, NULL)) break;
    CHECK(started == 2, "reader threads start");
    int bad = 0;
    for (int i = 0; i < 40 && !bad; ++i) {
        pid_t child = fork();
        if (child < 0) { bad = 1; break; }
        if (child == 0) {
            alarm(5);  // a child stuck on a lock the fork copied dies by SIGALRM instead of hanging the test
            size_t n = read_maps();
            char perms[5];
            _exit(n > 0 && line_covering((uintptr_t)&bad, perms) ? 0 : 1);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) bad = 1;
    }
    CHECK(!bad, "40 forks while two threads read maps: each child reads maps and exits");
    readers_stop = 1;
    for (int i = 0; i < started; ++i) pthread_join(threads[i], NULL);
}

int main(void) {
    unchanged_reads();
    anonymous_mapping();
    replaced_mapping();
    file_mapping();
    heap_growth();
    churn();
    fork_while_reading();
    printf(failures ? "test_proc_maps_live: %d FAILED\n" : "test_proc_maps_live: ALL PASS\n", failures);
    return failures != 0;
}
