#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

// The /proc/self/maps line that starts at `start`, or 0.
static int maps_line(unsigned long start, char *line, size_t size) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    int found = 0;
    unsigned long low;
    while (!found && fgets(line, (int)size, maps))
        found = sscanf(line, "%lx-", &low) == 1 && low == start;
    fclose(maps);
    return found;
}
// fs/proc/task_mmu.c: "start-end perms offset major:minor inode", a
// trailing space, then the name padded to column 73.
static int expect_file_line(void *at, size_t length, const char *perms, unsigned long offset,
                            const struct stat *st, const char *path) {
    char line[4096], want[160];
    if (!maps_line((unsigned long)at, line, sizeof(line))) return 1;
    int n = snprintf(want, sizeof(want), "%08lx-%08lx %s %08lx %02x:%02x %lu ",
                     (unsigned long)at, (unsigned long)at + length, perms, offset,
                     major(st->st_dev), minor(st->st_dev), (unsigned long)st->st_ino);
    if (strncmp(line, want, (size_t)n)) { printf("maps: got %swant %s%s\n", line, want, path); return 1; }
    size_t column = n < 73 ? 73 : (size_t)n + 1;
    for (size_t i = (size_t)n; i < column; ++i) if (line[i] != ' ') return 1;
    if (strncmp(line + column, path, strlen(path)) || line[column + strlen(path)] != '\n') {
        printf("maps: name in %s", line);
        return 1;
    }
    return 0;
}
static int check_maps_names(void) {
    int failed = 0;
    const long page = sysconf(_SC_PAGESIZE);
    char path[] = "/tmp/bifrost_maps_XXXXXX", real[4096];
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    char *bytes = calloc(3, (size_t)page);
    struct stat st;
    if (!bytes || write(fd, bytes, 3 * (size_t)page) != 3 * page || fstat(fd, &st) ||
        !realpath(path, real)) failed = 1;
    free(bytes);
    // Private file mapping split by mprotect: offsets follow the pages.
    char *view = failed ? MAP_FAILED : mmap(0, 2 * (size_t)page, PROT_READ, MAP_PRIVATE, fd, page);
    if (view == MAP_FAILED || mprotect(view + page, (size_t)page, PROT_READ | PROT_WRITE)) failed = 1;
    else {
        failed |= expect_file_line(view, (size_t)page, "r--p", (unsigned long)page, &st, real);
        failed |= expect_file_line(view + page, (size_t)page, "rw-p", 2ul * (unsigned long)page, &st, real);
        munmap(view, 2 * (size_t)page);
    }
    char *shared = failed ? MAP_FAILED : mmap(0, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) failed = 1;
    else {
        failed |= expect_file_line(shared, (size_t)page, "rw-s", 0, &st, real);
        munmap(shared, (size_t)page);
    }
    close(fd);
    unlink(path);
    // A PROT_NONE reservation is listed without access and without a name.
    char *none = mmap(0, 4 * (size_t)page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char line[4096], want[96];
    if (none == MAP_FAILED || !maps_line((unsigned long)none, line, sizeof(line))) failed = 1;
    else {
        snprintf(want, sizeof(want), "%08lx-%08lx ---p 00000000 00:00 0 \n",
                 (unsigned long)none, (unsigned long)none + 4 * (unsigned long)page);
        if (strcmp(line, want)) { printf("maps: got %swant %s", line, want); failed = 1; }
    }
    if (none != MAP_FAILED) munmap(none, 4 * (size_t)page);
    // The executable's code is an executable mapping named by its path.
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    FILE *maps = fopen("/proc/self/maps", "r");
    int code = 0;
    unsigned long low, high, here = (unsigned long)&check_maps_names;
    char perms[5];
    if (n > 0 && maps) {
        exe[n] = '\0';
        while (fgets(line, sizeof(line), maps))
            if (sscanf(line, "%lx-%lx %4s", &low, &high, perms) == 3 && low <= here && here < high)
                code = perms[0] == 'r' && perms[2] == 'x' && strstr(line, exe) != 0;
    }
    if (maps) fclose(maps);
    if (!code) { printf("maps: code line lacks r-x and %s\n", exe); failed = 1; }
    return failed;
}

// getpid/getppid/getpgid/getsid agree with /proc/self/stat, /proc/<pid>/stat
// and /proc/self/status, and a process leader's TID is its PID.
static int status_field(const char *name) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    int value = -1;
    size_t n = strlen(name);
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, name, n) && line[n] == ':') value = atoi(line + n + 1);
    if (f) fclose(f);
    return value;
}
static int check_identity(void) {
    int failed = 0;
    char path[64], line[1024], comm[64] = "", stat_comm[64] = "";
    const int pid = getpid();
    if (pid != (int)gettid()) { printf("identity: tid %d != pid %d\n", (int)gettid(), pid); ++failed; }
    FILE *c = fopen("/proc/self/comm", "r");
    if (!c || !fgets(comm, sizeof(comm), c)) ++failed;
    if (c) fclose(c);
    comm[strcspn(comm, "\n")] = '\0';
    for (int own = 0; own < 2; ++own) {
        snprintf(path, sizeof(path), own ? "/proc/%d/stat" : "/proc/self/stat", pid);
        FILE *f = fopen(path, "r");
        int spid = -1, ppid = -1, pgrp = -1, sid = -1;
        if (!f || !fgets(line, sizeof(line), f)) { printf("identity: cannot read %s\n", path); ++failed; }
        else {
            char *open = strchr(line, '('), *close = strrchr(line, ')');
            spid = atoi(line);
            if (!open || !close || close < open ||
                sscanf(close + 2, "%*c %d %d %d", &ppid, &pgrp, &sid) != 3) ++failed;
            else snprintf(stat_comm, sizeof(stat_comm), "%.*s", (int)(close - open - 1), open + 1);
        }
        if (f) fclose(f);
        if (spid != pid || ppid != getppid() || pgrp != getpgid(0) || sid != getsid(0) ||
            strcmp(stat_comm, comm)) {
            printf("identity: %s pid=%d ppid=%d pgrp=%d sid=%d comm=%s; want %d %d %d %d %s\n", path,
                   spid, ppid, pgrp, sid, stat_comm, pid, getppid(), getpgid(0), getsid(0), comm);
            ++failed;
        }
    }
    if (status_field("Pid") != pid || status_field("Tgid") != pid || status_field("PPid") != getppid()) {
        printf("identity: status Pid=%d Tgid=%d PPid=%d\n", status_field("Pid"),
               status_field("Tgid"), status_field("PPid"));
        ++failed;
    }
    return failed;
}
static int check_identity_both(void) {
    int failed = check_identity();
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) _exit(check_identity() ? 1 : 0);
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status)) ++failed;
    return failed;
}

int main(void) {
    int failed = 0;
    const char *names[] = {"/proc/self/exe", "/proc/1/exe"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        int fd = open(names[i], O_RDONLY | O_CLOEXEC);
        unsigned char header[4] = {0};
        if (fd < 0 || read(fd, header, sizeof(header)) != 4 || memcmp(header, "\177ELF", 4)) ++failed;
        if (fd >= 0) close(fd);
    }
    int fd = open("/proc/self/exe", O_PATH | O_CLOEXEC);
    if (fd < 0) ++failed;
    else {
        char link[64], target[4096], expected[4096];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, target, sizeof(target));
        ssize_t e = readlink("/proc/self/exe", expected, sizeof(expected));
        if (n <= 0 || n != e || memcmp(target, expected, n)) ++failed;
        close(fd);
    }
    errno = 0;
    fd = open("/proc/self/exe", O_WRONLY | O_TRUNC);
    if (fd >= 0 || errno != ETXTBSY) ++failed;
    if (fd >= 0) close(fd);
    errno = 0;
    fd = open("/proc/self/exe", O_RDONLY | O_NOFOLLOW);
    if (fd >= 0 || errno != ELOOP) ++failed;
    if (fd >= 0) close(fd);
    fd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
    if (fd < 0 || !(fcntl(fd, F_GETFL) & O_DIRECTORY)) ++failed;
    if (fd >= 0) close(fd);
    errno = 0;
    fd = open("/proc/self/exe", O_RDONLY | O_DIRECTORY);
    if (fd >= 0 || errno != ENOTDIR) ++failed;
    if (fd >= 0) close(fd);
    unsigned long long initial_stack = 0;
    FILE *stat = fopen("/proc/self/stat", "r");
    if (!stat) ++failed;
    else {
        char line[4096];
        if (!fgets(line, sizeof(line), stat)) ++failed;
        else {
            char *cursor = strrchr(line, ')');
            if (!cursor || cursor[1] != ' ' || cursor[2] != 'R') ++failed;
            else {
                cursor += 3;
                for (unsigned field = 4; field <= 28; ++field) {
                    char *next;
                    unsigned long long value = strtoull(cursor, &next, 10);
                    if (next == cursor) { ++failed; break; }
                    if (field == 28) initial_stack = value;
                    cursor = next;
                }
            }
        }
        fclose(stat);
    }
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) ++failed;
    else {
        char line[512];
        unsigned long long previous = 0, start, end;
        uintptr_t stack_address = (uintptr_t)&failed;
        int found_stack = 0;
        while (fgets(line, sizeof(line), maps)) {
            if (sscanf(line, "%llx-%llx", &start, &end) != 2 || start < previous) {
                ++failed;
                break;
            }
            previous = start;
            if (start <= stack_address && stack_address < end &&
                start <= initial_stack && initial_stack < end && strstr(line, "[stack]"))
                found_stack = 1;
        }
        if (!found_stack) ++failed;
        fclose(maps);
    }
    failed += check_maps_names();
    failed += check_identity_both();
    printf("test_proc_exe: %s\n", failed ? "FAIL" : "ALL PASS");
    return failed ? 1 : 0;
}
