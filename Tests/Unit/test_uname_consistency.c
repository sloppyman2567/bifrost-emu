// test_uname_consistency.c — uname() and the /proc files that repeat it must agree.
//
// nodename is gethostname() and /proc/sys/kernel/hostname, release is /proc/sys/kernel/osrelease and the start of
// /proc/version, and the version string ends /proc/version. These hold on a real Linux kernel too, so build it natively to
// compare.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failures++; } } while (0)

static int read_line(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char *got = fgets(out, (int)size, f);
    fclose(f);
    if (!got) return 0;
    out[strcspn(out, "\n")] = 0;
    return 1;
}

int main(void) {
    struct utsname u;
    CHECK(uname(&u) == 0, "uname");
    char host[256] = "", release[256] = "", version[1024] = "";
    CHECK(gethostname(host, sizeof host) == 0, "gethostname");
    CHECK(strcmp(host, u.nodename) == 0, "gethostname equals the uname nodename");
    char proc_host[256] = "", proc_release[256] = "";
    CHECK(read_line("/proc/sys/kernel/hostname", proc_host, sizeof proc_host), "read /proc/sys/kernel/hostname");
    CHECK(strcmp(proc_host, u.nodename) == 0, "/proc/sys/kernel/hostname equals the uname nodename");
    CHECK(read_line("/proc/sys/kernel/osrelease", proc_release, sizeof proc_release), "read /proc/sys/kernel/osrelease");
    CHECK(strcmp(proc_release, u.release) == 0, "/proc/sys/kernel/osrelease equals the uname release");
    CHECK(read_line("/proc/version", version, sizeof version), "read /proc/version");
    snprintf(release, sizeof release, "Linux version %s ", u.release);
    CHECK(strncmp(version, release, strlen(release)) == 0, "/proc/version starts with the uname release");
    size_t vlen = strlen(version), ulen = strlen(u.version);
    CHECK(vlen >= ulen && strcmp(version + vlen - ulen, u.version) == 0, "/proc/version ends with the uname version");
    printf("nodename=%s release=%s\n", u.nodename, u.release);
    printf(failures ? "test_uname_consistency: %d FAILED\n" : "test_uname_consistency: ALL PASS\n", failures);
    return failures != 0;
}
