// test_group_exit.c — exit_group and fatal default signals end the WHOLE
// process from any thread (Linux do_group_exit), while the main thread
// calling pthread_exit leaves the process running.
//
// Bifrost stopped only the receiving thread: ART's self-SIGKILL
// (Process.killProcess) left its daemon threads running and the emulator
// waited for them forever; exit_group from a worker never stopped main.
// Threads created in a fork child also reported their tid as getpid(), so
// kill(getpid(), ...) from them targeted no process.
//
// Each case runs in a forked child; the parent allows 10 s. A fatal signal
// may show up as death by that signal (Linux) or as exit status 128+signo
// (Bifrost's convention for guest signal deaths).
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) failures++;
}

static void* sleeper(void* arg) {
    (void)arg;
    for (;;) sleep(1);
    return NULL;
}

static void* spinner(void* arg) {
    (void)arg;
    volatile unsigned long n = 0;
    for (;;) n++;
    return NULL;
}

static void* worker_kill(void* arg) {
    usleep(100 * 1000);
    kill(getpid(), (int)(long)arg);
    for (;;) sleep(1);
    return NULL;
}

static void* worker_exit_group(void* arg) {
    (void)arg;
    usleep(100 * 1000);
    syscall(SYS_exit_group, 7);
    return NULL;
}

static void* worker_exit_later(void* arg) {
    (void)arg;
    usleep(200 * 1000);
    exit(5);
    return NULL;
}

// Run `child` in a forked process; return its wait status, or -1 on timeout.
static int run(void (*child)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        child();
        _exit(99);
    }
    for (int i = 0; i < 1000; ++i) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return status;
        usleep(10 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static int died_by(int status, int signo) {
    return status != -1 &&
           ((WIFSIGNALED(status) && WTERMSIG(status) == signo) ||
            (WIFEXITED(status) && WEXITSTATUS(status) == 128 + signo));
}

static void main_sigkill(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, sleeper, NULL);
    pthread_create(&t2, NULL, spinner, NULL);
    usleep(100 * 1000);
    kill(getpid(), SIGKILL);
    for (;;) sleep(1);
}

static void worker_sigterm(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, spinner, NULL);
    pthread_create(&t2, NULL, worker_kill, (void*)(long)SIGTERM);
    for (;;) sleep(1);
}

static void worker_group_exit(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, sleeper, NULL);
    pthread_create(&t2, NULL, worker_exit_group, NULL);
    for (;;) sleep(1);
}

static void worker_group_exit_spinning_main(void) {
    pthread_t t;
    pthread_create(&t, NULL, worker_exit_group, NULL);
    volatile unsigned long n = 0;
    for (;;) n++;
}

static pid_t worker_pid;
static void* report_pid(void* arg) {
    (void)arg;
    worker_pid = getpid();
    return NULL;
}

// Threads of a forked child report the child's pid, not their own tid.
static void fork_child_thread_pid(void) {
    pthread_t t;
    pthread_create(&t, NULL, report_pid, NULL);
    pthread_join(t, NULL);
    _exit(worker_pid == getpid() ? 0 : 1);
}

static void main_thread_exits_first(void) {
    pthread_t t;
    pthread_create(&t, NULL, worker_exit_later, NULL);
    pthread_exit(NULL);
}

int main(void) {
    check(died_by(run(main_sigkill), SIGKILL),
          "main thread SIGKILLs itself with live threads");
    check(died_by(run(worker_sigterm), SIGTERM),
          "worker kill(getpid(), SIGTERM) ends the process");
    int status = run(worker_group_exit);
    check(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 7,
          "worker exit_group(7) while main sleeps");
    status = run(worker_group_exit_spinning_main);
    check(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 7,
          "worker exit_group(7) while main spins");
    status = run(fork_child_thread_pid);
    check(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork child: a new thread's getpid() equals main's");
    status = run(main_thread_exits_first);
    check(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 5,
          "main pthread_exit keeps the process until a worker exits");
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
