// test_proc_pid_status.c — /proc/<pid>/status of a process that is registered with the guest
// debugger (after a fork) is a complete Linux status file, with the same fields as /proc/self/status.
//
// The debugger's own view of a task has only name, state, ids, tracer and credentials, about 150
// bytes. Anti-tamper code (Umamusume's CrackProof reads /proc/<its pid>/status every 100 ms) parses
// Threads, VmRSS, the signal masks and Seccomp, so a short file there is not a Linux status file.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failures++; } } while (0)

static int read_file(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

static long field(const char *text, const char *key) {
    char needle[64];
    snprintf(needle, sizeof needle, "\n%s:", key);
    const char *at = strstr(text, needle);
    if (!at && strncmp(text, key, strlen(key)) == 0 && text[strlen(key)] == ':') at = text - 1;
    return at ? strtol(at + strlen(needle), NULL, 10) : -1;
}

static void check_status(const char *path, pid_t pid) {
    char text[8192];
    int n = read_file(path, text, sizeof text);
    CHECK(n > 600, path);
    static const char *const keys[] = {"Name", "State", "Tgid", "Pid", "PPid", "TracerPid", "Uid", "Gid", "VmSize",
                                       "VmRSS", "Threads", "SigPnd", "SigBlk", "SigIgn", "SigCgt", "CapEff",
                                       "Seccomp", "Cpus_allowed", "voluntary_ctxt_switches"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        char needle[64];
        snprintf(needle, sizeof needle, "%s:", keys[i]);
        if (!strstr(text, needle)) { printf("FAIL %s lacks %s\n", path, keys[i]); failures++; }
    }
    CHECK(field(text, "Pid") == pid, "Pid names the process");
    CHECK(field(text, "Tgid") == pid, "Tgid names the process");
    CHECK(field(text, "NSpid") == pid, "NSpid follows Pid");
    CHECK(field(text, "TracerPid") == 0, "TracerPid is 0");
    CHECK(field(text, "Threads") >= 1, "Threads is at least 1");
}

int main(void) {
    char path[64];
    // Before any fork /proc/<getpid()> is the same file as /proc/self.
    snprintf(path, sizeof path, "/proc/%d/status", getpid());
    check_status("/proc/self/status", getpid());
    check_status(path, getpid());
    // After a fork both parent and child are tasks of the guest family.
    pid_t child = fork();
    if (child == 0) {
        snprintf(path, sizeof path, "/proc/%d/status", getpid());
        check_status(path, getpid());
        check_status("/proc/self/status", getpid());
        _exit(failures ? 1 : 0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child checks");
    snprintf(path, sizeof path, "/proc/%d/status", getpid());
    check_status(path, getpid());
    printf(failures ? "test_proc_pid_status: FAILED\n" : "test_proc_pid_status: ALL PASS\n");
    return failures != 0;
}
