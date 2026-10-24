// test_posix_spawn.c — posix_spawn(3) (and so system(3) and popen(3)) used to fail with ENOSYS: both libcs
// start the child with clone(CLONE_VM|CLONE_VFORK). A copy-on-write fork stands in for the shared-memory
// vfork, so everything a spawn is used for must work: arguments, environment, file actions, attributes and
// the error for a program that does not exist (musl reports it over a close-on-exec pipe).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s (errno %d)\n", msg, errno); } } while (0)

// Child mode: --child=<what>. Exit status 0 means it saw what the parent promised.
static int child(char **argv) {
    const char *what = argv[1] + 8;
    if (!strcmp(what, "args")) return !strcmp(argv[2], "two words") && !strcmp(argv[3], "") && argv[4] == NULL ? 0 : 1;
    if (!strcmp(what, "env")) { const char *v = getenv("SPAWN_VAR"); return v && !strcmp(v, "from-envp") ? 0 : 1; }
    if (!strcmp(what, "stdout")) { printf("redirected-output\n"); fflush(stdout); return 0; }
    if (!strcmp(what, "closed")) return fcntl(atoi(argv[2]), F_GETFD) < 0 && errno == EBADF ? 0 : 1;
    if (!strcmp(what, "sigmask")) {
        sigset_t m; sigemptyset(&m); sigprocmask(SIG_BLOCK, NULL, &m);
        return sigismember(&m, SIGUSR1) == 1 ? 0 : 1;
    }
    if (!strcmp(what, "exit7")) return 7;
    return 99;
}

static int spawn_wait(const char *path, char *const argv[], char *const envp[],
                      const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr, int *rc) {
    pid_t pid = 0;
    *rc = posix_spawn(&pid, path, fa, attr, argv, envp);
    if (*rc != 0) return -1;
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strncmp(argv[1], "--child=", 8)) return child(argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) { perror("readlink"); return 1; }
    self[n] = 0;
    int rc, st;

    { char *av[] = {self, "--child=exit7", NULL};
      st = spawn_wait(self, av, environ, NULL, NULL, &rc);
      CHECK(rc == 0 && st == 7, "posix_spawn starts a program and its exit status comes back"); }
    { char *av[] = {self, "--child=args", "two words", "", NULL};
      st = spawn_wait(self, av, environ, NULL, NULL, &rc);
      CHECK(rc == 0 && st == 0, "arguments, including an empty one and one with a space, arrive intact"); }
    { char *av[] = {self, "--child=env", NULL}; char *ev[] = {"SPAWN_VAR=from-envp", NULL};
      st = spawn_wait(self, av, ev, NULL, NULL, &rc);
      CHECK(rc == 0 && st == 0, "the envp passed to posix_spawn reaches the child"); }

    // File actions: redirect the child's stdout to a file, and close a descriptor it would inherit.
    char dir[] = "/tmp/bifrost-spawn-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char out[600];
    snprintf(out, sizeof(out), "%s/out", dir);
    { posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
      posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      char *av[] = {self, "--child=stdout", NULL};
      st = spawn_wait(self, av, environ, &fa, NULL, &rc);
      posix_spawn_file_actions_destroy(&fa);
      char text[64] = {0}; int f = open(out, O_RDONLY); if (f >= 0) { (void)!read(f, text, sizeof(text) - 1); close(f); }
      CHECK(rc == 0 && st == 0 && !strcmp(text, "redirected-output\n"), "addopen redirects the child's stdout into a file"); }
    { int keep = open("/dev/null", O_RDONLY);
      posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
      posix_spawn_file_actions_addclose(&fa, keep);
      char num[16], *av[] = {self, "--child=closed", num, NULL};
      snprintf(num, sizeof(num), "%d", keep);
      st = spawn_wait(self, av, environ, &fa, NULL, &rc);
      posix_spawn_file_actions_destroy(&fa);
      close(keep);
      CHECK(rc == 0 && st == 0, "addclose closes a descriptor in the child"); }
    { posix_spawnattr_t attr; posix_spawnattr_init(&attr);
      sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGUSR1);
      posix_spawnattr_setsigmask(&attr, &blocked);
      posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
      char *av[] = {self, "--child=sigmask", NULL};
      st = spawn_wait(self, av, environ, NULL, &attr, &rc);
      posix_spawnattr_destroy(&attr);
      CHECK(rc == 0 && st == 0, "POSIX_SPAWN_SETSIGMASK sets the child's signal mask"); }

    // A program that does not exist is reported to the caller (musl: over a pipe, which needs close-on-exec).
    { pid_t pid = 0; char *av[] = {"no-such-program-xyz", NULL};
      rc = posix_spawnp(&pid, "no-such-program-xyz", NULL, NULL, av, environ);
      if (rc == 0) waitpid(pid, &st, 0);  // glibc reports the failure as the child's exit status 127
      CHECK(rc == ENOENT || (rc == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 127), "a missing program is reported as ENOENT (or exit status 127)"); }

    // Several at once, then reaped in any order.
    { pid_t pids[4]; int ok = 0;
      for (int i = 0; i < 4; i++) { char *av[] = {self, "--child=exit7", NULL}; if (posix_spawn(&pids[i], self, NULL, NULL, av, environ) == 0) ok++; }
      int good = 0; for (int i = 3; i >= 0; i--) { if (waitpid(pids[i], &st, 0) == pids[i] && WIFEXITED(st) && WEXITSTATUS(st) == 7) good++; }
      CHECK(ok == 4 && good == 4, "four concurrent spawns all run and are reaped"); }

    unlink(out); rmdir(dir);
    if (fails == 0) printf("test_posix_spawn: ALL PASS (%d checks)\n", checks);
    else printf("test_posix_spawn: %d/%d FAILED\n", fails, checks);
    return fails != 0;
}
