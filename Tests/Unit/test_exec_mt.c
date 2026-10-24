// test_exec_mt.c — a program that starts as an exec'd child of a multi-threaded process still gets
// a working JIT, including the preemption ticker its own second thread needs.
//
// The parent starts a thread (the JIT's ticker starts), forks, and the child execs this program in
// "child" mode. There a thread spins in a tight native loop until a signal sent to it by the main
// thread sets a flag; the handler can only run if the spinning thread is preempted, which needs the
// epoch ticker. A child that inherited the parent's "ticker is running" flag without the thread
// never preempts and the test hangs (the harness times it out).
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int stop_flag;
static void handler(int s) { (void)s; stop_flag = 1; }
static void *spinner(void *arg) { (void)arg; while (!stop_flag) { } return NULL; }
static void *idle(void *arg) { (void)arg; struct timespec t = {0, 50 * 1000 * 1000}; nanosleep(&t, NULL); return NULL; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "child")) {
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = handler; sigaction(SIGUSR1, &sa, NULL);
        pthread_t t; pthread_create(&t, NULL, spinner, NULL);
        struct timespec nap = {0, 100 * 1000 * 1000}; nanosleep(&nap, NULL);  // let it enter the loop
        pthread_kill(t, SIGUSR1);
        pthread_join(t, NULL);
        printf("child: spinner preempted and joined\n");
        return 0;
    }
    pthread_t t; pthread_create(&t, NULL, idle, NULL);  // the parent is multi-threaded
    pthread_join(t, NULL);
    char self[512]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1); self[n] = 0;
    pid_t p = fork();
    if (p == 0) { alarm(30); char *av[] = {self, "child", 0}; char *ev[] = {0}; execve(self, av, ev); _exit(127); }
    int st; waitpid(p, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { printf("test_exec_mt: ALL PASS\n"); return 0; }
    printf("test_exec_mt: FAIL (child status %d, signal %d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return 1;
}
