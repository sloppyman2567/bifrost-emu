// test_proc_cmdline.c — the argument block and /proc/self/cmdline follow
// Linux: argv strings are contiguous and ascending with the environment
// after them (fs/exec.c), and cmdline is read live from that block
// (fs/proc/base.c get_mm_cmdline), including the setproctitle overflow.
// /proc/<getpid()> names the same process as /proc/self.
//
// Android's app_main sizes argv as one block and Process.setArgV0 rewrites
// it with the package name; Unity reads /proc/<pid>/cmdline for its
// identifier and dereferenced NULL when that file was missing.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char** environ;
static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static long read_file(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0, n;
    while ((n = read(fd, out + total, size - total)) > 0) total += n;
    close(fd);
    return n < 0 ? -1 : total;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        // Re-exec with known arguments so the block layout is predictable.
        char* args[] = {argv[0], "alpha", "beta-gamma", NULL};
        execv("/proc/self/exe", args);
        execv(argv[0], args);
        perror("execv");
        return 1;
    }
    char* block = argv[0];
    char* end = argv[argc - 1] + strlen(argv[argc - 1]) + 1;
    int contiguous = 1;
    for (int i = 1; i < argc; i++)
        contiguous &= argv[i] == argv[i - 1] + strlen(argv[i - 1]) + 1;
    check(contiguous, "argv strings are contiguous and ascending");
    check(!environ[0] || environ[0] == end, "environment follows argv");

    char cmdline[8192], own[8192], path[64];
    long n = read_file("/proc/self/cmdline", cmdline, sizeof(cmdline));
    check(n == end - block && !memcmp(cmdline, block, n), "cmdline is the argv block");
    snprintf(path, sizeof(path), "/proc/%d/cmdline", getpid());
    check(read_file(path, own, sizeof(own)) == n && !memcmp(own, cmdline, n),
          "/proc/<getpid()>/cmdline is /proc/self/cmdline");

    // Process.setArgV0: clear the block and write the new name at its start.
    memset(block, 0, end - block);
    strcpy(block, "com.example.renamed");
    n = read_file("/proc/self/cmdline", cmdline, sizeof(cmdline));
    check(n == end - block && !strcmp(cmdline, "com.example.renamed") &&
          cmdline[n - 1] == 0, "cmdline reflects a rewritten argv block");

    // setproctitle: the block's final NUL is overwritten, so the title is
    // the string at the start, through its NUL.
    end[-1] = 'x';
    n = read_file("/proc/self/cmdline", cmdline, sizeof(cmdline));
    check(n == (long)strlen("com.example.renamed") + 1 &&
          !strcmp(cmdline, "com.example.renamed"), "overflowed block reads as one title");

    if (failures) {
        printf("FAIL (%d)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
