// test_fork_stuck_report.c — fixture for BIFROST_FORK_STUCK_SECONDS (run by Scripts/Testing/test_fork_stuck_report.sh).
//
//   stuck: the child blocks forever (as a child waiting for a thread that does not exist in it would);
//          the parent kills it after a few seconds.
//   exec:  the child execs this program in --child mode, which exits at once.
//   exit:  the child exits without execing (syscall 93 from its only thread).
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return 0;
    const int stuck = argc > 1 && !strcmp(argv[1], "stuck");
    const int exit_child = argc > 1 && !strcmp(argv[1], "exit");
    pid_t child = fork();
    if (child == 0) {
        if (exit_child) _exit(0);
        if (stuck) for (;;) pause();
        char *args[] = {argv[0], "--child", 0};
        execv(argv[0], args);
        _exit(127);
    }
    if (stuck) {
        struct timespec wait = {3, 0};
        nanosleep(&wait, 0);
        kill(child, SIGKILL);
    }
    int status = 0;
    waitpid(child, &status, 0);
    puts("test_fork_stuck_report: parent done");
    return 0;
}
