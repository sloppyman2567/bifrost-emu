// test_orphan_ppid.c — a process whose parent has exited sees a new parent.
//
// On Linux the orphan is adopted by init or the nearest subreaper, and getppid() and the parent
// pid in /proc/self/stat (field 4) both change to it. Run in the emulator (argument "init") the
// guest's init is pid 1 and the host's adopter is not a guest process, so the orphan must see 1.
// Anti-tamper code that double-forks waits for its parent to die and then checks /proc/<own pid>/stat.
//
// Plain C: build it natively as well (without the argument) to compare with the host kernel.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int stat_ppid(void) {
    char text[512];
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    const char *close = strrchr(text, ')');  // the command name may contain spaces
    int ppid = -1;
    if (!close || sscanf(close + 1, " %*c %d", &ppid) != 1) return -1;
    return ppid;
}

int main(int argc, char **argv) {
    const int expect_init = argc > 1 && strcmp(argv[1], "init") == 0;
    int result[2];
    if (pipe(result) != 0) return 2;
    pid_t child = fork();
    if (child == 0) {
        const pid_t helper = getpid();
        if (fork() == 0) {
            // Wait for the helper (our parent) to exit and the adoption to show.
            for (int i = 0; i < 100 && getppid() == helper; i++) {
                struct timespec ts = {0, 20 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
            int report[3] = {(int)helper, getppid(), stat_ppid()};
            if (write(result[1], report, sizeof report) != sizeof report) _exit(1);
            _exit(0);
        }
        _exit(0);  // the helper exits at once, orphaning the grandchild
    }
    waitpid(child, NULL, 0);
    int report[3] = {0, 0, 0};
    close(result[1]);
    int failures = 0;
    if (read(result[0], report, sizeof report) != sizeof report) { printf("FAIL no report from the orphan\n"); return 1; }
    printf("helper %d, orphan sees getppid %d, stat ppid %d\n", report[0], report[1], report[2]);
    if (report[1] == report[0]) { printf("FAIL getppid still names the dead parent\n"); failures++; }
    if (report[2] != report[1]) { printf("FAIL /proc/self/stat disagrees with getppid\n"); failures++; }
    if (expect_init && report[1] != 1) { printf("FAIL the orphan does not see init (pid 1)\n"); failures++; }
    printf(failures ? "test_orphan_ppid: FAILED\n" : "test_orphan_ppid: ALL PASS\n");
    return failures != 0;
}
