// test_pty.c — a guest creates and drives its own pseudo terminal.
//
// openpty/forkpty/posix_openpt need TIOCSPTLCK and TIOCGPTN (and TIOCGPTPEER on newer
// libcs) on the master, TIOCSWINSZ, and the tty control requests (TCFLSH, TCSBRK,
// TIOCOUTQ, TIOCPKT ...). The test calls them as raw kernel ioctls, so it does not
// depend on the libc it is built with, and follows with a forkpty-style child that
// calls setsid() and takes the slave as its controlling terminal. Request numbers and
// the expected behaviour are Linux's (include/uapi/asm-generic/ioctls.h, tty_io.c,
// pty.c); the same program passes under qemu-aarch64.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define K_TCSBRK 0x5409u
#define K_TCFLSH 0x540Bu
#define K_TIOCEXCL 0x540Cu
#define K_TIOCNXCL 0x540Du
#define K_TIOCSCTTY 0x540Eu
#define K_TIOCOUTQ 0x5411u
#define K_TIOCGWINSZ 0x5413u
#define K_TIOCSWINSZ 0x5414u
#define K_TIOCPKT 0x5420u
#define K_TIOCGSID 0x5429u
#define K_TIOCGPTN 0x80045430u
#define K_TIOCSPTLCK 0x40045431u
#define K_TIOCGPTLCK 0x80045439u
#define K_TIOCGEXCL 0x80045440u
#define K_TIOCGPTPEER 0x5441u
#define K_TCIFLUSH 0
#define K_TCOFLUSH 1

struct k_winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

static int checks, failed;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static long ioc(int fd, unsigned req, unsigned long arg) {
    long r = syscall(SYS_ioctl, fd, (unsigned long)req, arg);
    return r < 0 ? -errno : r;
}
static long iocp(int fd, unsigned req, void *p) { return ioc(fd, req, (unsigned long)p); }

// Ready to read: data, or (for a master whose slave closed) the hang-up that makes read fail with EIO.
static int readable(int fd, int ms) {
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, ms) > 0 && (p.revents & (POLLIN | POLLHUP));
}

static ssize_t read_some(int fd, char *buf, size_t n, int ms) {
    if (!readable(fd, ms)) return -2;
    return read(fd, buf, n);
}

int main(void) {
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    CHECK(m >= 0, "open /dev/ptmx: errno %d", errno);
    if (m < 0) { printf("%d checks passed, %d failed\n", checks - failed, failed); return 1; }

    int lock = -1, n = -1;
    CHECK(iocp(m, K_TIOCGPTLCK, &lock) == 0 && lock == 1, "a new master starts locked (got %d)", lock);
    CHECK(iocp(m, K_TIOCGPTN, &n) == 0 && n >= 0 && n < 100000, "TIOCGPTN -> %d", n);
    char name[48]; snprintf(name, sizeof name, "/dev/pts/%d", n);

    // While locked the slave cannot be opened (EIO, from pty_open); after unlocking it can.
    errno = 0;
    int locked_open = open(name, O_RDWR | O_NOCTTY);
    CHECK(locked_open < 0 && errno == EIO, "open of a locked slave: fd %d errno %d, want EIO", locked_open, errno);
    if (locked_open >= 0) close(locked_open);
    int zero = 0;
    CHECK(iocp(m, K_TIOCSPTLCK, &zero) == 0, "TIOCSPTLCK(0)");
    lock = -1;
    CHECK(iocp(m, K_TIOCGPTLCK, &lock) == 0 && lock == 0, "unlocked (got %d)", lock);
    int s = open(name, O_RDWR | O_NOCTTY);
    CHECK(s >= 0, "open %s: errno %d", name, errno);

    // TIOCGPTPEER opens the same slave without a path.
    int peer = (int)ioc(m, K_TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(peer >= 0, "TIOCGPTPEER -> %d", peer);
    if (peer >= 0) {
        CHECK((fcntl(peer, F_GETFD) & FD_CLOEXEC) != 0, "the peer fd must carry O_CLOEXEC");
        struct k_winsize w = {7, 9, 0, 0};
        CHECK(iocp(peer, K_TIOCSWINSZ, &w) == 0, "TIOCSWINSZ on the peer");
        struct k_winsize g = {0};
        CHECK(iocp(s, K_TIOCGWINSZ, &g) == 0 && g.ws_row == 7 && g.ws_col == 9,
              "slave sees the size set through the peer: %ux%u", g.ws_row, g.ws_col);
        close(peer);
    }
    CHECK(s >= 0, "no slave, the rest is skipped");
    if (s < 0) { printf("%d checks passed, %d failed\n", checks - failed, failed); return 1; }

    // Window size: set on the master, read on the slave.
    struct k_winsize w = {40, 100, 0, 0}, g = {0};
    CHECK(iocp(m, K_TIOCSWINSZ, &w) == 0, "TIOCSWINSZ on the master");
    CHECK(iocp(s, K_TIOCGWINSZ, &g) == 0 && g.ws_row == 40 && g.ws_col == 100, "size on the slave: %ux%u", g.ws_row, g.ws_col);

    // Data flows both ways. The slave is in canonical mode with echo on, the default.
    char buf[64];
    CHECK(write(m, "hello\n", 6) == 6, "write to the master");
    ssize_t r = read_some(s, buf, sizeof buf, 2000);
    CHECK(r == 6 && memcmp(buf, "hello\n", 6) == 0, "slave read %zd", r);
    r = read_some(m, buf, sizeof buf, 2000);                       // the echo of the input line
    CHECK(r >= 6 && memcmp(buf, "hello\r\n", 7) == 0, "master read of the echo: %zd", r);
    CHECK(write(s, "out\n", 4) == 4, "write to the slave");
    r = read_some(m, buf, sizeof buf, 2000);                       // ONLCR: \n becomes \r\n
    CHECK(r == 5 && memcmp(buf, "out\r\n", 5) == 0, "master read of the output: %zd", r);

    // tcflush: queued input is discarded; the value argument 0 (TCIFLUSH) must not be EFAULT.
    CHECK(write(m, "stale\n", 6) == 6, "write stale input");
    usleep(100000);
    CHECK(ioc(s, K_TCFLSH, K_TCIFLUSH) == 0, "TCFLSH(TCIFLUSH)");
    CHECK(!readable(s, 200), "flushed input must not be readable");
    read_some(m, buf, sizeof buf, 200);                            // drain the echo
    CHECK(ioc(s, K_TCFLSH, K_TCOFLUSH) == 0, "TCFLSH(TCOFLUSH)");
    int q = -1;
    CHECK(iocp(s, K_TIOCOUTQ, &q) == 0 && q >= 0, "TIOCOUTQ");
    CHECK(ioc(s, K_TCSBRK, 1) == 0, "TCSBRK(1) is tcdrain");

    // Exclusive mode.
    int ex = -1;
    CHECK(ioc(s, K_TIOCEXCL, 0) == 0, "TIOCEXCL");
    CHECK(iocp(s, K_TIOCGEXCL, &ex) == 0 && ex == 1, "TIOCGEXCL after TIOCEXCL: %d", ex);
    CHECK(ioc(s, K_TIOCNXCL, 0) == 0, "TIOCNXCL");

    // Packet mode: the first byte of every master read is a status byte (0 = data).
    int on = 1;
    CHECK(iocp(m, K_TIOCPKT, &on) == 0, "TIOCPKT");
    CHECK(write(s, "p", 1) == 1, "write in packet mode");
    r = read_some(m, buf, sizeof buf, 2000);
    CHECK(r == 2 && buf[0] == 0 && buf[1] == 'p', "packet read %zd: %02x %02x", r, buf[0], buf[1]);
    on = 0;
    CHECK(iocp(m, K_TIOCPKT, &on) == 0, "TIOCPKT off");

    // The kernel's checks come back as errno values, not crashes.
    CHECK(iocp(s, K_TIOCGPTN, &n) == -ENOTTY, "TIOCGPTN on a slave must be ENOTTY");
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    CHECK(iocp(p[0], K_TIOCGPTN, &n) == -ENOTTY, "TIOCGPTN on a pipe");
    CHECK(ioc(p[0], K_TCFLSH, K_TCIFLUSH) == -ENOTTY, "TCFLSH on a pipe");
    CHECK(iocp(p[0], K_TIOCSWINSZ, &w) == -ENOTTY, "TIOCSWINSZ on a pipe");
    close(p[0]); close(p[1]);

    // forkpty: the child leads a new session and takes the slave as its controlling terminal.
    pid_t c = fork();
    if (c == 0) {
        if (setsid() < 0) _exit(10);
        if (ioc(s, K_TIOCSCTTY, 0) != 0) _exit(11);
        pid_t sid = getsid(0);
        int tsid = -1;
        if (ioc(s, K_TIOCGSID, (unsigned long)&tsid) != 0) _exit(12);
        char msg[48]; int len = snprintf(msg, sizeof msg, "child sid%s\n", sid == tsid ? "-ok" : "-differs");
        if (write(s, msg, len) != len) _exit(13);
        _exit(0);
    }
    CHECK(c > 0, "fork");
    int status = 0;
    CHECK(waitpid(c, &status, 0) == c && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "forkpty child: status %#x (exit %d: 10 = setsid, 11 = TIOCSCTTY, 12 = TIOCGSID, 13 = write)",
          status, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    r = read_some(m, buf, sizeof buf, 2000);
    CHECK(r > 0 && strstr(buf, "child sid-ok") != 0, "the child's output reaches the master: %zd '%.*s'", r, r > 0 ? (int)r : 0, buf);

    // Closing the slave hangs the master up: reads return EIO (Linux) after the data is drained.
    close(s);
    errno = 0;
    r = read_some(m, buf, sizeof buf, 1000);
    if (r > 0) r = read_some(m, buf, sizeof buf, 1000);
    CHECK(r == -1 && errno == EIO, "master read after the slave closed: %zd errno %d, want EIO", r, errno);
    close(m);

    printf("%d checks passed, %d failed\n", checks - failed, failed);
    return failed ? 1 : 0;
}
