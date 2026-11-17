// glibc's system() and popen() run `sh -c -- command` (POSIX: `--` ends the options). The rootfs shell is toybox, whose sh
// takes the word after -c as the command, so it ran "--" and every command answered 127 (see Exec.cpp,
// drop_dashdash_for_toybox_sh). Checks exit statuses, captured output, a command that starts with a dash (the reason glibc
// passes `--`) and a command with arguments. Without a /bin/sh in the rootfs there is nothing to run: skipped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    if (access("/bin/sh", X_OK) != 0) {
        puts("test_dyn_system: no /bin/sh in the rootfs, skipped: ALL PASS");
        return 0;
    }
    int status = system("echo from-system; exit 3");
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 3);
    status = system("exit 0");
    CHECK(status == 0);
    // A command beginning with '-' is the command, not an option: the shell cannot find "-nonexistent-" and says so.
    status = system("-nonexistent-command");
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 127);
    FILE* p = popen("echo from-popen arg2", "r");
    CHECK(p != NULL);
    if (p) {
        char line[128] = "";
        CHECK(fgets(line, sizeof line, p) != NULL && strcmp(line, "from-popen arg2\n") == 0);
        status = pclose(p);
        CHECK(status == 0);
    }
    p = popen("echo to-stderr 1>&2; exit 5", "r");
    CHECK(p != NULL);
    if (p) {
        char line[128];
        CHECK(fgets(line, sizeof line, p) == NULL);   // nothing on stdout
        status = pclose(p);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5);
    }
    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_dyn_system: %d checks: ALL PASS\n", checks);
    return 0;
}
