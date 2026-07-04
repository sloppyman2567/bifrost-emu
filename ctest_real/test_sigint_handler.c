// test_sigint_handler.c — Self-contained signal handler test (no pty needed).
//
// Verifies that when a signal arrives during a blocking read(), the
// guest's signal handler runs BEFORE read() returns -EINTR. This tests
// the Turn 43 signal delivery fix (handle_eintr + SVC PC advancement).
//
// How it works:
//   1. Install a SIGINT handler that sets a flag.
//   2. Fork a child process.
//   3. Child: usleep(200ms), then kill(getppid(), SIGINT), then _exit(0).
//   4. Parent: block on read(0, ...) — no input, so it blocks.
//   5. After 200ms, the child sends SIGINT to the parent (host process).
//   6. The emulator's host signal handler catches it and queues it.
//   7. The host read() returns -EINTR.
//   8. The read() syscall handler calls handle_eintr(), which drains the
//      pending SIGINT. The guest handler runs (setting the flag).
//   9. read() returns -EINTR to the guest.
//  10. Parent verifies: handler ran (flag set) AND read returned -EINTR.
//
// Build: make cross SRC=ctest_real/test_sigint_handler.c OUT=ctest_real/test_sigint_handler.elf
// Run:   ./bifrost-emu ctest_real/test_sigint_handler.elf
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static volatile sig_atomic_t got_sigint = 0;

static void handler(int sig) {
    (void)sig;
    got_sigint = 1;
    write(2, "HANDLER_RAN\n", 12);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART — read should return -EINTR
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        write(2, "FAIL: sigaction failed\n", 23);
        return 1;
    }

    // Fork a child that will send SIGINT after 200ms.
    pid_t child = fork();
    if (child < 0) {
        write(2, "FAIL: fork failed\n", 18);
        return 1;
    }
    if (child == 0) {
        // Child: wait 200ms, then signal parent.
        usleep(200000);
        kill(getppid(), SIGINT);
        _exit(0);
    }

    // Parent: block on read(). We use a pipe (not stdin) so the test
    // works even when stdin is /dev/null (as the test runner does).
    // The write end is kept open but never written to, so read() blocks
    // until the SIGINT from the child interrupts it.
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        write(2, "FAIL: pipe failed\n", 18);
        return 1;
    }
    write(2, "READING\n", 8);
    char buf[16];
    ssize_t n = read(pipefd[0], buf, sizeof(buf));

    {
        char msg[128];
        int len = snprintf(msg, sizeof(msg),
                           "AFTER_READ: n=%zd errno=%d got_sigint=%d\n",
                           n, errno, (int)got_sigint);
        write(2, msg, len);
    }

    // Reap the child.
    waitpid(child, NULL, 0);

    if (n < 0 && errno == EINTR) {
        if (got_sigint) {
            write(2, "PASS: handler ran before read returned -EINTR\n", 46);
            return 0;
        } else {
            write(2, "FAIL: read returned -EINTR but handler did not run\n", 51);
            return 1;
        }
    } else if (n >= 0) {
        write(2, "FAIL: read was not interrupted\n", 31);
        return 1;
    } else {
        char msg[64];
        int len = snprintf(msg, sizeof(msg), "FAIL: n=%zd errno=%d\n", n, errno);
        write(2, msg, len);
        return 1;
    }
}
