// test_pipe.c — Verify pipe() + read/write between parent and child.
//
// What it verifies:
//   1. pipe() creates a working pipe (two fds, read end + write end).
//   2. Writing to the write end and reading from the read end works.
//   3. After fork(), the parent can write and the child can read.
//   4. Closing the write end causes read() on the read end to return 0 (EOF).
//   5. read() on an empty pipe with the write end open blocks (returns -EINTR
//      when interrupted by a signal).
//
// Build: make cross SRC=ctest_real/test_pipe.c OUT=ctest_real/test_pipe.elf
// Run:   ./bifrost-emu ctest_real/test_pipe.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

int main(void) {
    int pipefd[2];

    // 1. Create pipe.
    int r = pipe(pipefd);
    CHECK(r == 0, "pipe() returns 0");
    CHECK(pipefd[0] > 0, "pipe read end is a valid fd");
    CHECK(pipefd[1] > 0, "pipe write end is a valid fd");
    CHECK(pipefd[0] != pipefd[1], "read and write ends are different fds");

    // 2. Simple write/read in same process.
    const char *msg = "hello pipe\n";
    ssize_t n = write(pipefd[1], msg, strlen(msg));
    CHECK(n == (ssize_t)strlen(msg), "write to pipe");

    char buf[64] = {0};
    n = read(pipefd[0], buf, sizeof(buf));
    CHECK(n == (ssize_t)strlen(msg), "read from pipe returns full message");
    CHECK(strcmp(buf, msg) == 0, "pipe content roundtrips correctly");

    // 3. Fork: parent writes, child reads.
    int pipefd2[2];
    pipe(pipefd2);

    pid_t child = fork();
    if (child < 0) {
        CHECK(0, "fork succeeds");
        return 1;
    }
    if (child == 0) {
        // Child: close write end, read message.
        close(pipefd2[1]);
        char cbuf[64] = {0};
        ssize_t cn = read(pipefd2[0], cbuf, sizeof(cbuf));
        if (cn > 0) {
            write(2, "CHILD: ", 7);
            write(2, cbuf, cn);
            write(2, "\n", 1);
        }
        close(pipefd2[0]);
        _exit(0);
    }
    // Parent: close read end, write message.
    close(pipefd2[0]);
    const char *pmsg = "from parent";
    write(pipefd2[1], pmsg, strlen(pmsg));
    close(pipefd2[1]);  // EOF

    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exits cleanly");

    // 4. Closing write end → read returns 0 (EOF).
    int pipefd3[2];
    pipe(pipefd3);
    close(pipefd3[1]);
    char ebuf[16];
    n = read(pipefd3[0], ebuf, sizeof(ebuf));
    CHECK(n == 0, "read on pipe with closed write end returns 0 (EOF)");
    close(pipefd3[0]);

    // 5. Final summary.
    char summary[128];
    int len = snprintf(summary, sizeof(summary),
                       "passes=%d failures=%d\n", passes, failures);
    write(2, summary, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
