// Guest ptrace: real stop/wait ownership, memory, AArch64 regsets and stepping.
// Also build natively to compare the process/memory contract with Linux.
#define _GNU_SOURCE
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#endif
static volatile long word = 0x12345678;
static volatile long* readonly_word;
static int failures;
static void check(int ok, const char* label) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) { printf("  errno=%d\n", errno); ++failures; }
}
static void byte_write(int fd) { char b = 1; while (write(fd, &b, 1) < 0 && errno == EINTR) {} }
static int byte_read(int fd) {
    char b; ssize_t n;
    do { n = read(fd, &b, 1); } while (n < 0 && errno == EINTR);
    return n == 1;
}
static void traced_child(void) {
    if (ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(90);
    raise(SIGSTOP);
#ifdef __aarch64__
    __asm__ volatile(".global ptrace_step_start\nptrace_step_start:\nmov x9, #41\nadd x9, x9, #1\n" ::: "x9");
#endif
    _exit(word == 0x76543210 && *readonly_word == 0x11223344 ? 23 : 24);
}
static void traceme(void) {
    readonly_word = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *readonly_word = 7;
    mprotect((void*)readonly_word, 4096, PROT_READ);
    sigset_t blocked, saved; sigemptyset(&blocked); sigaddset(&blocked, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blocked, &saved);
    pid_t child = fork();
    if (!child) traced_child();
    siginfo_t notification = {0}; struct timespec timeout = {3, 0};
    check(sigtimedwait(&blocked, &notification, &timeout) == SIGCHLD &&
          notification.si_pid == child && notification.si_code == CLD_TRAPPED && notification.si_status == SIGSTOP,
          "SIGCHLD carries the trace stop identity and status");
    int status = 0;
    int stopped = waitpid(child, &status, 0) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP;
    check(stopped, "TRACEME produces a real wait stop");
    if (!stopped) { kill(child, SIGKILL); waitpid(child, 0, 0); return; }
    errno = 0;
    check(ptrace(PTRACE_PEEKDATA, child, (void*)&word, 0) == 0x12345678 && !errno, "PEEK reads tracee memory");
    check(ptrace(PTRACE_POKEDATA, child, (void*)&word, (void*)0x76543210) == 0, "POKE writes tracee memory");
    check(ptrace(PTRACE_POKEDATA, child, (void*)readonly_word, (void*)0x11223344) == 0,
          "POKE writes a private read-only tracee page");
    errno = 0;
    check(ptrace(PTRACE_PEEKDATA, child, (void*)-8, 0) == -1 && errno == EIO, "PEEK rejects addresses outside guest memory");
    siginfo_t info;
    check(ptrace(PTRACE_GETSIGINFO, child, 0, &info) == 0 && info.si_signo == SIGSTOP, "GETSIGINFO describes the stop");
    errno = 0;
    check(ptrace(PTRACE_GETSIGINFO, child, 0, (void*)-4096) == -1 && errno == EFAULT, "GETSIGINFO validates its guest output");
    errno = 0;
    check(ptrace(PTRACE_SETOPTIONS, child, 0, (void*)0x80000000UL) == -1 && errno == EINVAL, "unknown ptrace options fail");
#ifdef __aarch64__
    extern char ptrace_step_start[];
    uint64_t regs[34];
    struct iovec iov = {regs, sizeof(regs)};
    check(ptrace(PTRACE_GETREGSET, child, (void*)1, &iov) == 0 && iov.iov_len == 272, "AArch64 GPR layout and size");
    regs[32] = (uintptr_t)ptrace_step_start;
    check(ptrace(PTRACE_SETREGSET, child, (void*)1, &iov) == 0, "SETREGSET redirects guest PC");
    uint8_t fp[528]; struct iovec fp_iov = {fp, sizeof(fp)};
    check(ptrace(PTRACE_GETREGSET, child, (void*)2, &fp_iov) == 0 && fp_iov.iov_len == sizeof(fp), "AArch64 FPSIMD layout and size");
    uint64_t value = 0x1020304050607080ULL; memcpy(fp, &value, 8);
    check(ptrace(PTRACE_SETREGSET, child, (void*)2, &fp_iov) == 0, "write guest SIMD register");
    memset(fp, 0, sizeof(fp));
    check(ptrace(PTRACE_GETREGSET, child, (void*)2, &fp_iov) == 0 && !memcmp(fp, &value, 8), "read back guest SIMD register");
    uint64_t tls; struct iovec tls_iov = {&tls, 8};
    check(ptrace(PTRACE_GETREGSET, child, (void*)0x401, &tls_iov) == 0 && tls_iov.iov_len == 8, "read guest TLS regset");
    check(ptrace(PTRACE_SETREGSET, child, (void*)0x401, &tls_iov) == 0, "restore guest TLS regset");
#endif
    check(ptrace(PTRACE_SINGLESTEP, child, 0, 0) == 0, "SINGLESTEP resumes tracee");
    check(waitpid(child, &status, 0) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP, "SINGLESTEP delivers a trap stop");
#ifdef __aarch64__
    iov.iov_len = sizeof(regs);
    check(ptrace(PTRACE_GETREGSET, child, (void*)1, &iov) == 0 &&
          regs[32] == (uintptr_t)ptrace_step_start + 4 && regs[9] == 41, "single step executes exactly one guest instruction");
#endif
    check(ptrace(PTRACE_CONT, child, 0, 0) == 0, "CONT resumes tracee");
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23, "wait observes tracee exit and changed memory");
    errno = 0;
    check(ptrace(PTRACE_CONT, child, 0, 0) == -1 && errno == ESRCH, "dead tracee rejected");
    check(word == 0x12345678, "debugger does not modify its own CoW memory");
    check(*readonly_word == 7, "forced tracee write preserves parent read-only mapping");
    munmap((void*)readonly_word, 4096);
    sigprocmask(SIG_SETMASK, &saved, NULL);
}
static void tracer_death(int exitkill) {
    pid_t victim = fork();
    if (!victim) {
        int ready[2], done[2]; pipe(ready); pipe(done);
        pid_t parent = getpid(), tracer = fork();
        if (!tracer) {
            int status;
            if (!byte_read(ready[0]) || ptrace(PTRACE_ATTACH, parent, 0, 0) ||
                waitpid(parent, &status, 0) != parent || !WIFSTOPPED(status)) _exit(92);
            if (exitkill && ptrace(PTRACE_SETOPTIONS, parent, 0, (void*)0x100000)) _exit(93);
            byte_write(done[1]);
            _exit(0);
        }
        prctl(PR_SET_PTRACER, tracer, 0, 0, 0); byte_write(ready[1]);
        int ok = byte_read(done[0]), status;
        ok = ok && waitpid(tracer, &status, 0) == tracer && WIFEXITED(status) && !WEXITSTATUS(status);
        _exit(ok ? 0 : 94);
    }
    int status;
    int ok = waitpid(victim, &status, 0) == victim;
    check(ok && (exitkill ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL :
                WIFEXITED(status) && !WEXITSTATUS(status)),
          exitkill ? "EXITKILL kills tracee when tracer exits" : "tracer exit releases a stopped tracee");
}
static void attach_parent(void) {
    int ready[2], done[2]; pipe(ready); pipe(done);
    pid_t parent = getpid();
    pid_t child = fork();
    if (!child) {
        int ok = byte_read(ready[0]);
        ok = ok && ptrace(PTRACE_ATTACH, parent, 0, 0) == 0;
        int status;
        ok = ok && waitpid(parent, &status, 0) == parent && WIFSTOPPED(status);
        char path[64], buf[1024] = {0}, expected[64];
        snprintf(path, sizeof(path), "/proc/%d/status", parent);
        FILE* file = fopen(path, "r");
        if (file) { fread(buf, 1, sizeof(buf) - 1, file); fclose(file); }
        snprintf(expected, sizeof(expected), "TracerPid:\t%d\n", getpid());
        ok = ok && strstr(buf, expected) != NULL && strstr(buf, "t (tracing stop)") != NULL;
        ok = ok && ptrace(PTRACE_DETACH, parent, 0, 0) == 0;
        byte_write(done[1]);
        _exit(ok ? 0 : 91);
    }
    // Native Linux with Yama requires the parent to authorize its child.
    prctl(PR_SET_PTRACER, child, 0, 0, 0);
    byte_write(ready[1]);
    check(byte_read(done[0]), "attach/detach wakes a tracee blocked in read");
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "child traces parent and sees real TracerPid");
    close(ready[0]); close(ready[1]); close(done[0]); close(done[1]);
}
static void seize(void) {
    int ready[2]; pipe(ready);
    pid_t child = fork();
    if (!child) { byte_write(ready[1]); for (;;) pause(); }
    byte_read(ready[0]);
    check(ptrace(PTRACE_SEIZE, child, 0, 0) == 0, "SEIZE establishes ownership without stopping");
    int status = 0;
    check(waitpid(child, &status, WNOHANG) == 0, "SEIZE leaves target running");
    check(ptrace(PTRACE_INTERRUPT, child, 0, 0) == 0, "INTERRUPT requests a seized-task stop");
    siginfo_t info = {0};
    check(waitid(P_PID, child, &info, WSTOPPED | WNOWAIT) == 0 &&
          info.si_pid == child && info.si_code == CLD_TRAPPED && info.si_status == (SIGTRAP | (128 << 8)),
          "waitid WNOWAIT reports a tracing stop");
    check(waitpid(child, &status, 0) == child && WIFSTOPPED(status) &&
          WSTOPSIG(status) == SIGTRAP && (unsigned)status >> 16 == 128, "wait consumes PTRACE_EVENT_STOP");
    check(ptrace(PTRACE_DETACH, child, 0, 0) == 0, "detach seized task");
    kill(child, SIGKILL);
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "detached child exits by signal");
    close(ready[0]); close(ready[1]);
}
static volatile sig_atomic_t handled;
static void usr1(int signo) { (void)signo; ++handled; }
static void signal_stops(void) {
    pid_t child = fork();
    if (!child) {
        signal(SIGUSR1, usr1);
        if (ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(95);
        raise(SIGSTOP); raise(SIGUSR1); raise(SIGUSR1);
        _exit(handled == 1 ? 0 : 96);
    }
    int status, ok = waitpid(child, &status, 0) == child && WIFSTOPPED(status);
    ok = ok && ptrace(PTRACE_CONT, child, 0, 0) == 0;
    ok = ok && waitpid(child, &status, 0) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGUSR1;
    ok = ok && ptrace(PTRACE_CONT, child, 0, 0) == 0;
    ok = ok && waitpid(child, &status, 0) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGUSR1;
    ok = ok && ptrace(PTRACE_CONT, child, 0, (void*)(uintptr_t)SIGUSR1) == 0;
    ok = ok && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
    check(ok, "tracer suppresses one signal and delivers the next");
    if (!ok) { kill(child, SIGKILL); waitpid(child, NULL, 0); }
}
static void exec_stop(void) {
    pid_t child = fork();
    if (!child) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(97);
        raise(SIGSTOP);
        execl("/proc/self/exe", "test_ptrace", "--exec-child", (char*)NULL);
        _exit(98);
    }
    int status, ok = waitpid(child, &status, 0) == child && WIFSTOPPED(status);
    ok = ok && ptrace(PTRACE_CONT, child, 0, 0) == 0;
    ok = ok && waitpid(child, &status, 0) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP;
    ok = ok && ptrace(PTRACE_CONT, child, 0, 0) == 0;
    ok = ok && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37;
    check(ok, "traced exec stops before running the new image");
    if (!ok) { kill(child, SIGKILL); waitpid(child, NULL, 0); }
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--exec-child")) return 37;
    setbuf(stdout, NULL); alarm(20);
    traceme(); attach_parent(); seize(); tracer_death(0); tracer_death(1); signal_stops(); exec_stop();
    errno = 0;
    check(ptrace(PTRACE_ATTACH, getpid(), 0, 0) == -1 && errno == EPERM, "cannot attach to own thread group");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    puts("ALL PASS"); return 0;
}
