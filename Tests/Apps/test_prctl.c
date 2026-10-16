// test_prctl.c — Test prctl syscall (PR_SET_NAME, PR_GET_NAME, etc.)
//
// bifrost-emu previously returned 0 for ALL prctl options, which broke
// PR_GET_NAME (returned garbage) and made programs that probe prctl
// features think they succeeded when they didn't. This test verifies:
//   1. PR_SET_NAME / PR_GET_NAME round-trip works
//   2. PR_GET_NAME returns the ELF basename by default
//   3. PR_SET_NAME truncates to 15 chars (kernel limit)
//   4. PR_GET_DUMPABLE returns 1 (SUID_DUMP_USER)
//   5. PR_GET_NO_NEW_PRIVS returns 0 (not set)
//   6. PR_GET_TIMERSLACK returns a positive value
//   7. Unknown prctl options return -EINVAL
//   8. PR_GET_TID_ADDRESS works
//   9. Task names are per thread: PR_SET_NAME names the caller, threads
//      inherit their creator's name, /proc/self/comm is the leader's and
//      /proc/self/task/<tid>/comm reads and renames one thread
//
// Build: make cross SRC=Tests/Apps/test_prctl.c OUT=Tests/Apps/test_prctl.elf
// Run:   ./bifrost-emu Tests/Apps/test_prctl.elf
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

static int read_text(const char* path, char* out, size_t size) {
    memset(out, 0, size);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, size - 1);
    close(fd);
    return n < 0 ? -1 : 0;
}

static pthread_barrier_t names_barrier;
static long worker_tid;
static char worker_inherited[16], worker_self_comm[32], worker_renamed[16];

static void* name_worker(void* arg) {
    (void)arg;
    worker_tid = syscall(SYS_gettid);
    prctl(PR_GET_NAME, worker_inherited, 0, 0, 0);
    prctl(PR_SET_NAME, "worker", 0, 0, 0);
    read_text("/proc/thread-self/comm", worker_self_comm, sizeof(worker_self_comm));
    pthread_barrier_wait(&names_barrier);  // main inspects and renames us
    pthread_barrier_wait(&names_barrier);
    prctl(PR_GET_NAME, worker_renamed, 0, 0, 0);
    return NULL;
}

int main(void) {
    char msg[256];
    int n;

    // 1. PR_GET_NAME — should return the ELF basename by default.
    //    The test binary is "test_prctl.elf", so comm should start with
    //    "test_prctl" (truncated to 15 chars if needed).
    char name[17] = {0};
    int r = prctl(PR_GET_NAME, name, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NAME default: r=%d name=\"%s\"\n", r, name);
    write(2, msg, n);
    CHECK(r == 0, "PR_GET_NAME returns 0");
    CHECK(strncmp(name, "test_prctl", 10) == 0, "PR_GET_NAME returns ELF basename");

    // 2. PR_SET_NAME — set a custom name, then verify PR_GET_NAME returns it.
    r = prctl(PR_SET_NAME, "mytestprog", 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_SET_NAME: r=%d\n", r);
    write(2, msg, n);
    CHECK(r == 0, "PR_SET_NAME returns 0");

    memset(name, 0, sizeof(name));
    r = prctl(PR_GET_NAME, name, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NAME after set: r=%d name=\"%s\"\n", r, name);
    write(2, msg, n);
    CHECK(r == 0 && strcmp(name, "mytestprog") == 0, "PR_GET_NAME returns set name");

    // 3. PR_SET_NAME truncates to 15 chars (kernel limit).
    prctl(PR_SET_NAME, "this_is_a_very_long_process_name", 0, 0, 0);
    memset(name, 0, sizeof(name));
    prctl(PR_GET_NAME, name, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NAME after long set: name=\"%s\" len=%zu\n", name, strlen(name));
    write(2, msg, n);
    CHECK(strlen(name) <= 15, "PR_SET_NAME truncates to 15 chars");

    // 4. PR_GET_DUMPABLE — should return 1 (SUID_DUMP_USER).
    r = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_DUMPABLE: r=%d\n", r);
    write(2, msg, n);
    CHECK(r >= 0, "PR_GET_DUMPABLE returns non-negative");

    // 5. PR_GET_NO_NEW_PRIVS — should return 0 (not set).
    r = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NO_NEW_PRIVS: r=%d\n", r);
    write(2, msg, n);
    CHECK(r == 0, "PR_GET_NO_NEW_PRIVS returns 0");

    // 6. PR_GET_TIMERSLACK — should return a positive value.
    r = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_TIMERSLACK: r=%d\n", r);
    write(2, msg, n);
    CHECK(r > 0, "PR_GET_TIMERSLACK returns positive");

    // 7. Unknown prctl option — should return -EINVAL.
    //    Use a value that's definitely not a valid PR_* constant.
    r = prctl(0xFFFF, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "prctl(0xFFFF): r=%d errno=%d\n", r, errno);
    write(2, msg, n);
    CHECK(r == -1 && errno == EINVAL, "Unknown prctl returns -EINVAL");

    // 8. PR_GET_TID_ADDRESS — should write the tid_address to the pointer.
    unsigned long tid_addr = 0xDEADBEEF;
    r = prctl(PR_GET_TID_ADDRESS, &tid_addr, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_TID_ADDRESS: r=%d tid_addr=0x%lx\n", r, tid_addr);
    write(2, msg, n);
    CHECK(r == 0, "PR_GET_TID_ADDRESS returns 0");

    // 9. Per-thread task names.
    {
        char text[32], path[64];
        pthread_t thread;
        prctl(PR_SET_NAME, "leader", 0, 0, 0);
        pthread_barrier_init(&names_barrier, NULL, 2);
        pthread_create(&thread, NULL, name_worker, NULL);
        pthread_barrier_wait(&names_barrier);
        CHECK(strcmp(worker_inherited, "leader") == 0, "new thread inherits its creator's name");
        CHECK(strcmp(worker_self_comm, "worker\n") == 0, "/proc/thread-self/comm shows the caller");
        memset(name, 0, sizeof(name));
        prctl(PR_GET_NAME, name, 0, 0, 0);
        CHECK(strcmp(name, "leader") == 0, "a worker's PR_SET_NAME leaves the leader");
        CHECK(read_text("/proc/self/comm", text, sizeof(text)) == 0 && strcmp(text, "leader\n") == 0,
              "/proc/self/comm shows the leader");
        snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", worker_tid);
        CHECK(read_text(path, text, sizeof(text)) == 0 && strcmp(text, "worker\n") == 0,
              "/proc/self/task/<tid>/comm reads the thread");
        int fd = open(path, O_WRONLY);
        CHECK(fd >= 0 && write(fd, "renamed", 7) == 7, "/proc/self/task/<tid>/comm accepts a write");
        if (fd >= 0) close(fd);
        pthread_barrier_wait(&names_barrier);
        pthread_join(thread, NULL);
        CHECK(strcmp(worker_renamed, "renamed") == 0, "a task comm write renames that thread");
        errno = 0;
        CHECK(open("/proc/self/task/999999/comm", O_RDONLY) < 0 && errno == ENOENT,
              "an unknown task has no comm");
    }

    // Final summary.
    n = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, n);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
