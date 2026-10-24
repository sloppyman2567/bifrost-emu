// test_execve.c — execve semantics a shell, make or posix_spawn relies on.
//
// The program re-executes itself (and a copy of itself) in child modes; the parent asserts on exit
// statuses. Covers:
//   - envp reaches the new image (and NULL means an empty environment);
//   - FD_CLOEXEC descriptors close across exec, others (including a dup of one) stay;
//   - "#!" scripts run their interpreter with [interp, arg, script, args...], also nested;
//   - errno: ENOENT, EACCES (directory, non-executable), ENOEXEC (no "#!"), ELOOP (shebang loop);
//   - arguments and environment strings longer than 4096 bytes arrive intact;
//   - /proc/self/exe names the program that was exec'd, and AT_RANDOM is not all zeros.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s (errno %d)\n", msg, errno); } } while (0)

// ── child modes: exit 0 when what they observe is right ────────────────────────────────────
static int child(int argc, char **argv) {
    const char *mode = argv[1] + 8;  // after "--child="
    if (!strcmp(mode, "env")) {      // argv[2] = expected value of EXEC_VAR
        const char *v = getenv("EXEC_VAR");
        return v && !strcmp(v, argv[2]) ? 0 : 1;
    }
    if (!strcmp(mode, "emptyenv")) return environ && environ[0] == NULL ? 0 : 1;
    if (!strcmp(mode, "fdclosed")) return fcntl(atoi(argv[2]), F_GETFD) < 0 && errno == EBADF ? 0 : 1;
    if (!strcmp(mode, "fdopen")) return fcntl(atoi(argv[2]), F_GETFD) >= 0 ? 0 : 1;
    if (!strcmp(mode, "longarg")) return strlen(argv[2]) == 100000 && argv[2][99999] == 'z' ? 0 : 1;
    if (!strcmp(mode, "longenv")) { const char *v = getenv("LONG_VAR"); return v && strlen(v) == 50000 ? 0 : 1; }
    if (!strcmp(mode, "exe")) {      // argv[2] = expected /proc/self/exe
        char b[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", b, sizeof(b) - 1);
        if (n < 0) return 1; b[n] = 0;
        return strcmp(b, argv[2]) == 0 ? 0 : 1;
    }
    if (!strcmp(mode, "random")) {
        const unsigned char *r = (const unsigned char *)getauxval(AT_RANDOM);
        if (!r) return 1;
        for (int i = 0; i < 16; i++) if (r[i]) return 0;
        return 1;
    }
    if (!strcmp(mode, "script")) {
        // The interpreter's view: [self, --child=script, <script>, args...]. Run directly the last
        // argument is "extra"; through a second script that names this one, argv[3] is that script.
        if (argc == 4) return !strcmp(argv[3], "extra") ? 0 : 1;
        if (argc == 5) { const char *s2 = strrchr(argv[3], '/'); return s2 && !strcmp(s2, "/script2") && !strcmp(argv[4], "extra") ? 0 : 1; }
        return 1;
    }
    return 99;
}

// Run a program in a forked child; returns its exit status (or -1 / 128+sig), `err` its execve errno (0 = it ran).
static int run(const char *path, char *const argv[], char *const envp[], int *err) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return -1;
    pid_t p = fork();
    if (p == 0) {
        execve(path, argv, envp);
        int e = errno;
        (void)!write(pipefd[1], &e, sizeof(e));
        _exit(127);
    }
    close(pipefd[1]);
    int e = 0;
    ssize_t n = read(pipefd[0], &e, sizeof(e));  // EOF when exec succeeded (the pipe is close-on-exec)
    close(pipefd[0]);
    *err = n == (ssize_t)sizeof(e) ? e : 0;
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void write_file(const char *path, const char *text, mode_t mode) {
    int f = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (f >= 0) { (void)!write(f, text, strlen(text)); close(f); }
    chmod(path, mode);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strncmp(argv[1], "--child=", 8)) return child(argc, argv);
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) { perror("readlink"); return 1; }
    self[n] = 0;
    char dir[] = "/tmp/bifrost-execve-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    int err, st;

    // envp
    { char *av[] = {self, "--child=env", "custom-value", 0}; char *ev[] = {"EXEC_VAR=custom-value", 0};
      st = run(self, av, ev, &err); CHECK(err == 0 && st == 0, "envp reaches the new image"); }
    { char *av[] = {self, "--child=emptyenv", 0};
      st = run(self, av, NULL, &err); CHECK(err == 0 && st == 0, "a NULL envp is an empty environment"); }

    // FD_CLOEXEC
    int a = open("/dev/null", O_RDONLY | O_CLOEXEC), b = open("/dev/null", O_RDONLY);
    int c = dup(a), d = fcntl(b, F_DUPFD_CLOEXEC, 10), e = open("/dev/null", O_RDONLY);
    fcntl(e, F_SETFD, FD_CLOEXEC);
    int pfd[2]; pipe2(pfd, O_CLOEXEC);
    CHECK(fcntl(a, F_GETFD) == FD_CLOEXEC && fcntl(b, F_GETFD) == 0 && fcntl(c, F_GETFD) == 0 &&
          fcntl(d, F_GETFD) == FD_CLOEXEC && fcntl(e, F_GETFD) == FD_CLOEXEC, "F_GETFD reports FD_CLOEXEC per descriptor");
    struct { const char *name, *mode; int fd; } fdcases[] = {
        {"open(O_CLOEXEC) closes", "fdclosed", a}, {"a plain open stays open", "fdopen", b},
        {"dup() of a close-on-exec descriptor stays open", "fdopen", c}, {"F_DUPFD_CLOEXEC closes", "fdclosed", d},
        {"F_SETFD(FD_CLOEXEC) closes", "fdclosed", e}, {"pipe2(O_CLOEXEC) closes", "fdclosed", pfd[0]}};
    for (unsigned i = 0; i < sizeof(fdcases) / sizeof(*fdcases); i++) {
        char mode[32], num[16]; snprintf(mode, sizeof(mode), "--child=%s", fdcases[i].mode); snprintf(num, sizeof(num), "%d", fdcases[i].fd);
        char *av[] = {self, mode, num, 0}; char *ev[] = {0};
        st = run(self, av, ev, &err); CHECK(err == 0 && st == 0, fdcases[i].name);
    }

    // scripts: the interpreter is this program, which reports through its exit status
    char script[PATH_MAX], script2[PATH_MAX], loop[PATH_MAX], text[PATH_MAX * 2];
    snprintf(script, sizeof(script), "%s/script", dir);
    snprintf(text, sizeof(text), "#!%s --child=script\nignored\n", self);
    write_file(script, text, 0755);
    { char *av[] = {script, "extra", 0}; char *ev[] = {0};
      st = run(script, av, ev, &err); CHECK(err == 0 && st == 0, "#! script runs its interpreter with [interp, arg, script, args]"); }
    snprintf(script2, sizeof(script2), "%s/script2", dir);
    snprintf(text, sizeof(text), "#!%s\n", script);   // interpreter is itself a script
    write_file(script2, text, 0755);
    { char *av[] = {script2, "extra", 0}; char *ev[] = {0};
      // [self, --child=script, script, script2, extra]: each interpreter hands over the script it was run for.
      st = run(script2, av, ev, &err); CHECK(err == 0 && st == 0, "nested #! passes each script path through"); }
    snprintf(loop, sizeof(loop), "%s/loop", dir);
    snprintf(text, sizeof(text), "#!%s\n", loop);
    write_file(loop, text, 0755);
    { char *av[] = {loop, 0}; char *ev[] = {0};
      st = run(loop, av, ev, &err); CHECK(err == ELOOP, "a #! loop is ELOOP"); }

    // errno
    { char *av[] = {"/nonexistent/prog", 0}; char *ev[] = {0};
      run("/nonexistent/prog", av, ev, &err); CHECK(err == ENOENT, "a missing file is ENOENT (no redirect to this program)"); }
    { char *av[] = {dir, 0}; char *ev[] = {0};
      run(dir, av, ev, &err); CHECK(err == EACCES, "a directory is EACCES"); }
    char plain[PATH_MAX], noexec[PATH_MAX], badinterp[PATH_MAX];
    snprintf(plain, sizeof(plain), "%s/plain", dir);
    write_file(plain, "echo no shebang\n", 0755);
    { char *av[] = {plain, 0}; char *ev[] = {0};
      run(plain, av, ev, &err); CHECK(err == ENOEXEC, "an executable text file without #! is ENOEXEC"); }
    snprintf(noexec, sizeof(noexec), "%s/noexec", dir);
    write_file(noexec, "#!/bin/sh\n", 0644);
    { char *av[] = {noexec, 0}; char *ev[] = {0};
      run(noexec, av, ev, &err); CHECK(err == EACCES, "a file without execute permission is EACCES"); }
    snprintf(badinterp, sizeof(badinterp), "%s/badinterp", dir);
    write_file(badinterp, "#!/nonexistent/interpreter\n", 0755);
    { char *av[] = {badinterp, 0}; char *ev[] = {0};
      run(badinterp, av, ev, &err); CHECK(err == ENOENT, "a #! script whose interpreter is missing is ENOENT"); }

    // long arguments and environment
    { char *big = malloc(100001); memset(big, 'x', 100000); big[99999] = 'z'; big[100000] = 0;
      char *av[] = {self, "--child=longarg", big, 0}; char *ev[] = {0};
      st = run(self, av, ev, &err); CHECK(err == 0 && st == 0, "a 100000-byte argument arrives intact");
      free(big); }
    { char *var = malloc(50000 + 10); strcpy(var, "LONG_VAR="); memset(var + 9, 'y', 50000); var[9 + 50000] = 0;
      char *av[] = {self, "--child=longenv", 0}; char *ev[] = {var, 0};
      st = run(self, av, ev, &err); CHECK(err == 0 && st == 0, "a 50000-byte environment string arrives intact");
      free(var); }

    // a different program: /proc/self/exe follows exec
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s/copy-of-self", dir);
    { int in = open(self, O_RDONLY), out = open(copy, O_CREAT | O_WRONLY | O_TRUNC, 0755); char buf[65536]; ssize_t r;
      while (in >= 0 && out >= 0 && (r = read(in, buf, sizeof(buf))) > 0) (void)!write(out, buf, (size_t)r);
      if (in >= 0) close(in); if (out >= 0) close(out); chmod(copy, 0755); }
    { char *real = realpath(copy, NULL); char *av[] = {copy, "--child=exe", real, 0}; char *ev[] = {0};
      st = run(copy, av, ev, &err); CHECK(err == 0 && st == 0, "/proc/self/exe names the program that was exec'd");
      free(real); }
    { char *av[] = {self, "--child=random", 0}; char *ev[] = {0};
      st = run(self, av, ev, &err); CHECK(err == 0 && st == 0, "AT_RANDOM is not all zeros after exec"); }

    unlink(script); unlink(script2); unlink(loop); unlink(plain); unlink(noexec); unlink(badinterp); unlink(copy);
    rmdir(dir);
    if (fails == 0) printf("test_execve: ALL PASS (%d checks)\n", checks);
    else printf("test_execve: %d/%d FAILED\n", fails, checks);
    return fails != 0;
}
