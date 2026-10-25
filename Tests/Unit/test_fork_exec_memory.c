// test_fork_exec_memory.c — a fork child that execs must not keep a copy of the parent's memory.
//
// The parent maps and touches 256 MiB, forks, and the child execs this program in --idle mode, where it sleeps.
// Scripts/Testing/test_fork_exec_memory.sh reads the child's private memory from the host's /proc while it sleeps.
// Writing zeros over the old image before the exec used to copy every page the child shared with its parent
// (and allocate a host buffer as large as each mapping), so a game that ran one helper command briefly doubled
// in size per command.
//
// Build: make setup-tests
// Run:   Scripts/Testing/test_fork_exec_memory.sh
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PARENT_BYTES (256UL << 20)

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--idle")) {
        puts("EXECED");
        fflush(stdout);
        struct timespec pause = {5, 0};
        nanosleep(&pause, 0);
        return 0;
    }
    char *memory = mmap(0, PARENT_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { puts("FAIL: mmap"); return 1; }
    for (unsigned long i = 0; i < PARENT_BYTES; i += 4096) memory[i] = (char)(i >> 12) | 1;
    pid_t child = fork();
    if (child == 0) {
        char *args[] = {argv[0], "--idle", 0};
        execv(argv[0], args);
        _exit(127);
    }
    printf("CHILD %d\n", (int)child);
    fflush(stdout);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: child status 0x%x\n", status);
        return 1;
    }
    puts("test_fork_exec_memory: child exited");
    return 0;
}
