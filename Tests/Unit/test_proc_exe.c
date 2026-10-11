#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    printf("test_proc_exe: %s\n", failed ? "FAIL" : "ALL PASS");
    return failed ? 1 : 0;
}
