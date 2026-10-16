// A memory fault in a worker thread is a synchronous signal for that thread
// (Linux arch/arm64/mm/fault.c: arm64_force_sig_fault on current). Its SIGSEGV
// handler runs with si_addr set, and the thread survives returning from it.
// ART relies on this for implicit null checks on every thread.
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>

static __thread sigjmp_buf jump;
static void *page;
static volatile int fault_signo;
static void *volatile fault_addr;

static void handler(int signo, siginfo_t *info, void *context) {
    (void)context;
    fault_signo = signo;
    fault_addr = info->si_addr;
    siglongjmp(jump, 1);
}

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        if (!sigsetjmp(jump, 1)) {
            (void)*(volatile int *)page;
            return "load did not fault";
        }
        if (fault_signo != SIGSEGV || fault_addr != page) return "wrong signal or si_addr";
        fault_signo = 0;
        fault_addr = 0;
    }
    return NULL;
}

int main(void) {
    struct sigaction action = {0};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL)) return 1;
    page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || munmap(page, 4096)) return 1;
    pthread_t thread;
    void *failure;
    if (pthread_create(&thread, NULL, worker, NULL) || pthread_join(thread, &failure)) return 1;
    if (failure) {
        printf("FAIL: %s\n", (const char *)failure);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
