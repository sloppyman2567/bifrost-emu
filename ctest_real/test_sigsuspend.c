// test_sigsuspend.c — Test sigsuspend woken by a forked child's signal.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static volatile sig_atomic_t got_sig = 0;

static void handler(int sig) {
    (void)sig;
    got_sig = 1;
    write(2, "HANDLER\n", 8);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    pid_t child = fork();
    if (child < 0) { write(2, "FORK_FAIL\n", 10); return 1; }
    if (child == 0) {
        // Child: sleep 100ms, then signal parent.
        usleep(100000);
        write(2, "CHILD: sending SIGUSR1\n", 23);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    // Parent: sigsuspend with empty mask (all unblocked).
    write(2, "PARENT: sigsuspend\n", 19);
    sigset_t empty;
    sigemptyset(&empty);
    got_sig = 0;
    int r = sigsuspend(&empty);
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "PARENT: sigsuspend returned %d errno=%d got_sig=%d\n", r, errno, (int)got_sig);
    write(2, msg, n);

    waitpid(child, NULL, 0);

    if (got_sig) {
        write(2, "PASS\n", 5);
        return 0;
    }
    write(2, "FAIL\n", 5);
    return 1;
}
