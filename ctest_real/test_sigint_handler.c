// test_sigint_handler.c — Verify that a real SIGINT handler runs when
// Ctrl+C is pressed during a blocking read.
//
// Installs a SIGINT handler that sets a flag, then blocks on read().
// When SIGINT arrives, the handler should run (setting the flag), and
// then read() should return -EINTR. The program verifies the flag is
// set BEFORE read() returns.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

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
        perror("sigaction");
        return 1;
    }

    write(2, "READING\n", 8);

    char buf[16];
    ssize_t n = read(0, buf, sizeof(buf));

    {
        char msg[128];
        int len = snprintf(msg, sizeof(msg),
                           "AFTER_READ: n=%zd errno=%d got_sigint=%d\n",
                           n, errno, (int)got_sigint);
        write(2, msg, len);
    }

    if (n < 0 && errno == EINTR) {
        if (got_sigint) {
            write(2, "PASS: handler ran before read returned -EINTR\n", 46);
            return 0;
        } else {
            write(2, "FAIL: read returned -EINTR but handler did not run\n", 51);
            return 1;
        }
    } else if (n > 0) {
        char msg[64];
        int len = snprintf(msg, sizeof(msg),
                           "FAIL: read succeeded, got %zd bytes: 0x%02x\n", n, (unsigned)buf[0]);
        write(2, msg, len);
        return 1;
    } else if (n == 0) {
        write(2, "FAIL: read returned 0 (EOF)\n", 28);
        return 1;
    } else {
        char msg[64];
        int len = snprintf(msg, sizeof(msg), "FAIL: n=%zd errno=%d\n", n, errno);
        write(2, msg, len);
        return 1;
    }
}
