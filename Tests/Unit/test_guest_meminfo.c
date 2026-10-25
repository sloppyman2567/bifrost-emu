/* /proc/meminfo and sysinfo agree and are consistent: toybox's field order, free <= available <= total,
 * buffers + cached + free <= total, no swap, and (with argv[1], in kB) the exact total the policy gives. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>

static int failed;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); failed = 1; } } while (0)

int main(int argc, char **argv) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { puts("FAIL: /proc/meminfo"); return 1; }
    static const char *order[8] = {"MemTotal:", "MemFree:", "Buffers:", "Cached:", "Shmem:", "SwapTotal:", "SwapFree:", "SwapCached:"};
    unsigned long long total = 0, freekb = 0, buffers = 0, cached = 0, shmem = 0, avail = 0, swap = ~0ull;
    char line[256], name[64];
    for (int i = 0; fgets(line, sizeof(line), f); i++) {
        unsigned long long kb;
        if (sscanf(line, "%63s %llu", name, &kb) != 2) continue;
        if (i < 8) CHECK(!strcmp(name, order[i]), "first eight fields in toybox order");
        if (!strcmp(name, "MemTotal:")) total = kb;
        else if (!strcmp(name, "MemFree:")) freekb = kb;
        else if (!strcmp(name, "MemAvailable:")) avail = kb;
        else if (!strcmp(name, "Buffers:")) buffers = kb;
        else if (!strcmp(name, "Cached:")) cached = kb;
        else if (!strcmp(name, "Shmem:")) shmem = kb;
        else if (!strcmp(name, "SwapTotal:")) swap = kb;
    }
    fclose(f);
    CHECK(total > 0 && avail > 0, "total and available are reported");
    CHECK(avail <= total, "available <= total");
    CHECK(freekb <= avail, "free <= available");
    CHECK(buffers + cached + freekb <= total, "buffers + cached + free <= total");
    CHECK(shmem <= total, "shmem <= total");
    CHECK(swap == 0, "no swap");
    struct sysinfo si;
    CHECK(sysinfo(&si) == 0, "sysinfo");
    unsigned long long unit = si.mem_unit ? si.mem_unit : 1;
    CHECK(si.totalram * unit == total * 1024ull, "sysinfo total equals MemTotal");
    CHECK(si.freeram * unit == freekb * 1024ull || si.freeram * unit <= total * 1024ull, "sysinfo free is within the total");
    CHECK(si.totalswap == 0 && si.freeswap == 0, "sysinfo reports no swap");
    if (argc > 1) CHECK(total == strtoull(argv[1], NULL, 10), "total is the expected capped value");
    if (failed) return 1;
    printf("test_guest_meminfo: ALL PASS (MemTotal %llu kB, MemAvailable %llu kB)\n", total, avail);
    return 0;
}
