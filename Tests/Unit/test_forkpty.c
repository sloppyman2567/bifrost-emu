// test_forkpty.c — the libc view of a guest-created pseudo terminal.
//
// posix_openpt/grantpt/unlockpt/ptsname_r, tcgetattr/tcsetattr (TCGETS2 on glibc 2.42+),
// TIOCSWINSZ, tcflush/tcdrain, openpty and forkpty, with an exec of the static toybox and an
// interactive shell on the pty. test_pty.c checks the kernel ABI directly; this checks that
// the libc functions programs call (script, ssh, tmux, python's pty module) work end to end.
// argv[1] is the toybox path (default Tests/Apps/toybox, relative to the repository root).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <poll.h>
#include <pty.h>
#include <utmp.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static size_t slurp(int fd, char *buf, size_t cap, int ms_total) {
  size_t n = 0; int waited = 0;
  while (n < cap - 1 && waited < ms_total) {
    struct pollfd p = {fd, POLLIN, 0};
    int r = poll(&p, 1, 100);
    if (r == 0) { waited += 100; continue; }
    ssize_t k = read(fd, buf + n, cap - 1 - n);
    if (k <= 0) break;
    n += k;
  }
  buf[n] = 0; return n;
}
int main(int argc, char **argv) {
  const char *toybox = argc > 1 ? argv[1] : "Tests/Apps/toybox";
  /* 1. the POSIX sequence */
  int m = posix_openpt(O_RDWR | O_NOCTTY);
  CHECK(m >= 0, "posix_openpt errno=%d", errno);
  CHECK(grantpt(m) == 0, "grantpt errno=%d", errno);
  CHECK(unlockpt(m) == 0, "unlockpt errno=%d", errno);
  char name[64] = "";
  CHECK(ptsname_r(m, name, sizeof name) == 0 && strncmp(name, "/dev/pts/", 9) == 0, "ptsname_r '%s' errno=%d", name, errno);
  int s = open(name, O_RDWR | O_NOCTTY);
  CHECK(s >= 0, "open slave '%s' errno=%d", name, errno);
  CHECK(isatty(s) == 1, "isatty(slave)");
  struct termios t; CHECK(tcgetattr(s, &t) == 0, "tcgetattr");
  cfmakeraw(&t); CHECK(tcsetattr(s, TCSANOW, &t) == 0, "tcsetattr raw");
  struct termios u; CHECK(tcgetattr(s, &u) == 0 && !(u.c_lflag & (ECHO | ICANON)), "raw mode took effect");
  struct winsize w = {24, 80, 0, 0}; CHECK(ioctl(m, TIOCSWINSZ, &w) == 0, "TIOCSWINSZ");
  struct winsize g; CHECK(ioctl(s, TIOCGWINSZ, &g) == 0 && g.ws_row == 24 && g.ws_col == 80, "TIOCGWINSZ %ux%u", g.ws_row, g.ws_col);
  CHECK(tcflush(s, TCIOFLUSH) == 0, "tcflush errno=%d", errno);
  CHECK(tcdrain(s) == 0, "tcdrain errno=%d", errno);
  close(s); close(m);
  /* 2. openpty */
  int om, os; char oname[64];
  CHECK(openpty(&om, &os, oname, NULL, &w) == 0, "openpty errno=%d", errno);
  CHECK(write(om, "x", 1) == 1, "write master");
  close(om); close(os);
  /* 3. forkpty + exec */
  int fm; pid_t pid = forkpty(&fm, NULL, NULL, &w);
  if (pid == 0) {
    execl(toybox, "toybox", "echo", "pty-ok", (char*)0);
    _exit(127);
  }
  CHECK(pid > 0, "forkpty errno=%d", errno);
  char buf[256]; size_t n = slurp(fm, buf, sizeof buf, 5000);
  int st = 0; waitpid(pid, &st, 0);
  CHECK(strstr(buf, "pty-ok") != 0, "child output via master: '%s'", buf);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child status %#x", st);
  close(fm);
  /* 4. forkpty + an interactive shell: send a command, get its output, ^C, exit */
  pid = forkpty(&fm, NULL, NULL, &w);
  if (pid == 0) { execl(toybox, "toybox", "sh", (char*)0); _exit(127); }
  CHECK(pid > 0, "forkpty sh");
  usleep(500000);
  write(fm, "echo shell-$((20+22))\n", 22);
  n = slurp(fm, buf, sizeof buf, 3000);
  CHECK(strstr(buf, "shell-42") != 0, "shell output '%s'", buf);
  write(fm, "exit 5\n", 7);
  n = slurp(fm, buf, sizeof buf, 3000);
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 5, "shell exit status %#x", st);
  printf(fails ? "%d FAILED\n" : "test_forkpty: ALL PASS\n", fails);
  return fails != 0;
}
