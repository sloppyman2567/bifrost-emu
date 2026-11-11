// test_poll_signals.c — poll and select as Linux returns them when a signal arrives during the wait.
//
// 1. A child exits during the wait while SIGCHLD is ignored by default: no handler runs, so ppoll and pselect6 keep
//    waiting and time out (0). The emulator's host wait is interrupted by the host SIGCHLD; it used to return -1,
//    which the guest reads as EPERM (a session Zygote died of it, taking its children with it).
//    pselect6's timeout is a timespec: it waited 1000 times too long when its nanoseconds were read as microseconds.
// 2. With a SIGCHLD handler installed, both return EINTR after the handler ran.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static volatile sig_atomic_t handled;
#define CHECK(cond, msg) do { if (cond) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); failures++; } } while (0)

static void on_child(int signal) { (void)signal; handled = 1; }

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// Waits 400 ms on a pipe nobody writes while a child exits after 50 ms. Returns the call's result; *error and
// *seconds report errno and the time spent.
static int wait_during_child_exit(int use_select, int* error, double* seconds) {
    int pipe_fds[2];
    if (pipe(pipe_fds)) return -2;
    pid_t child = fork();
    if (child == 0) {
        usleep(50000);
        _exit(0);
    }
    double start = now();
    int result;
    if (use_select) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(pipe_fds[0], &read_set);
        struct timespec timeout = {0, 400000000};
        result = pselect(pipe_fds[0] + 1, &read_set, NULL, NULL, &timeout, NULL);
    } else {
        struct pollfd entry = {pipe_fds[0], POLLIN, 0};
        struct timespec timeout = {0, 400000000};
        result = ppoll(&entry, 1, &timeout, NULL);
    }
    *error = result < 0 ? errno : 0;
    *seconds = now() - start;
    waitpid(child, NULL, 0);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return result;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);  // fork children must not repeat buffered lines
    for (int use_select = 0; use_select < 2; use_select++) {
        const char* name = use_select ? "pselect6" : "ppoll";
        int error;
        double seconds;
        char message[160];
        signal(SIGCHLD, SIG_DFL);
        int result = wait_during_child_exit(use_select, &error, &seconds);
        snprintf(message, sizeof message, "%s keeps waiting through an ignored SIGCHLD (result %d, %s, %.2f s)",
                 name, result, result < 0 ? strerror(error) : "-", seconds);
        CHECK(result == 0 && seconds >= 0.35 && seconds < 2.0, message);  // a timespec, not microseconds
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_handler = on_child;  // no SA_RESTART: poll and select are never restarted anyway
        sigaction(SIGCHLD, &action, NULL);
        handled = 0;
        result = wait_during_child_exit(use_select, &error, &seconds);
        snprintf(message, sizeof message, "%s returns EINTR after a SIGCHLD handler ran (result %d, %s)",
                 name, result, result < 0 ? strerror(error) : "-");
        CHECK(result == -1 && error == EINTR && handled, message);
    }
    signal(SIGCHLD, SIG_DFL);
    if (failures) {
        printf("test_poll_signals: %d FAILED\n", failures);
        return 1;
    }
    printf("test_poll_signals: ALL PASS\n");
    return 0;
}
