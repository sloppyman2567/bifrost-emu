// test_sysfs_cpu.c — the synthetic /sys CPU topology.
//
// libunity.so sizes its job-system worker pool from
// sysconf(_SC_NPROCESSORS_ONLN), which Bionic backs with
// /sys/devices/system/cpu/online. The emulator has no kernel
// sysfs, so the file was missing, sysconf returned -1, and
// Unity's worker count computed to 0 — the presentation
// thread then spun on a zero batch size forever (window
// black, cursor trails, audio still running).
//
// Asserts every guest-visible CPU-count source agrees on
// kGuestCpuCount (8) CPUs:
//   - sysconf(_SC_NPROCESSORS_ONLN)
//   - /sys/devices/system/cpu/{online,possible,present} = "0-7\n"
//   - stat() of the topology files: S_IFREG, size 4
//   - getdents64 on /sys/devices/system/cpu: 3 files + 8 cpu dirs
//   - sched_getaffinity(0, ...): 8 CPUs (unchanged behavior)
//
// Plain C. Emulator-only: the assertions describe the
// synthetic tree (8 CPUs), not the host machine's real
// /sys, so there is no native run to compare with.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_getdents64
#define SYS_getdents64 61
#endif

// linux_dirent64 as served by the emulator's getdents64.
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void check_file(const char* path, const char* want) {
    char what[128];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    snprintf(what, sizeof what, "open %s", path);
    check(fd >= 0, what);
    if (fd < 0) return;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    snprintf(what, sizeof what, "%s read", path);
    if (n <= 0) { check(0, what); return; }
    buf[n] = '\0';
    snprintf(what, sizeof what, "%s is \"%s\"", path, want);
    check(strcmp(buf, want) == 0, what);
}

static void check_stat_reg(const char* path, off_t want_size) {
    char what[128];
    struct stat st;
    int ok = fstatat(AT_FDCWD, path, &st, 0) == 0 &&
             S_ISREG(st.st_mode) && st.st_size == want_size;
    snprintf(what, sizeof what, "stat %s: S_IFREG size %ld",
             path, (long)want_size);
    check(ok, what);
}

int main(void) {
    // 1. sysconf — the value Unity's job system consumes.
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    char what[64];
    snprintf(what, sizeof what, "sysconf(_SC_NPROCESSORS_ONLN) == %d",
             8);
    check(ncpu == 8, what);

    // 2. The topology files Bionic and Unity read.
    check_file("/sys/devices/system/cpu/online",   "0-7\n");
    check_file("/sys/devices/system/cpu/possible", "0-7\n");
    check_file("/sys/devices/system/cpu/present",  "0-7\n");

    // 3. Synthetic stat must agree with the open path.
    check_stat_reg("/sys/devices/system/cpu/online", 4);
    check_stat_reg("/sys/devices/system/cpu/possible", 4);
    check_stat_reg("/sys/devices/system/cpu/present", 4);
    check(access("/sys/devices/system/cpu/online", R_OK) == 0,
          "faccessat(online, R_OK)");

    // 4. Directory listing: 3 topology files + 8 cpu dirs.
    int fd = open("/sys/devices/system/cpu",
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(fd >= 0, "open /sys/devices/system/cpu as directory");
    if (fd < 0) return 1;
    int nfiles = 0, ncpus = 0;
    char buf[4096];
    for (;;) {
        ssize_t n = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t off = 0; off < n;) {
            struct linux_dirent64* d =
                (struct linux_dirent64*)(buf + off);
            off += d->d_reclen;
            if (!strcmp(d->d_name, ".") ||
                !strcmp(d->d_name, ".."))
                continue;
            if (d->d_type == 1 /*DT_REG*/ &&
                (!strcmp(d->d_name, "online") ||
                 !strcmp(d->d_name, "possible") ||
                 !strcmp(d->d_name, "present"))) {
                nfiles++;
            } else if (d->d_type == 4 /*DT_DIR*/ &&
                       d->d_name[0] == 'c' &&
                       d->d_name[1] == 'p' &&
                       d->d_name[2] == 'u') {
                ncpus++;
            } else {
                char bad[96];
                snprintf(bad, sizeof bad,
                         "unexpected dirent %s (type %u)",
                         d->d_name, d->d_type);
                check(0, bad);
            }
        }
    }
    close(fd);
    snprintf(what, sizeof what,
             "getdents64 lists 3 topology files (got %d)", nfiles);
    check(nfiles == 3, what);
    snprintf(what, sizeof what,
             "getdents64 lists 8 cpu dirs (got %d)", ncpus);
    check(ncpus == 8, what);

    // 5. sched_getaffinity still reports 8 CPUs.
    cpu_set_t set;
    CPU_ZERO(&set);
    int r = sched_getaffinity(0, sizeof set, &set);
    check(r == 0 && CPU_COUNT(&set) == 8,
          "sched_getaffinity(0) reports 8 CPUs");

    // Per-CPU leaves Unity also strings-greps for.
    check_file("/sys/devices/system/cpu/cpu0/cpu_capacity",
               "1024\n");
    check_file("/sys/devices/system/cpu/cpu7/cpufreq/cpuinfo_max_freq",
               "1800000\n");

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
