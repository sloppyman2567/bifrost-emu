/* test_input.c — Test the /dev/input/eventX input device.
 *
 * Opens /dev/input/event0, reads input_event records, and prints them.
 * Exits after reading 5 events or after 1 second of no input.
 *
 * Under bifrost-emu with SDL2: the SDL2 event loop captures keyboard
 * and mouse events and translates them to Linux input_event records.
 * Without SDL2 (headless): the input device is always empty, so this
 * test will print "no events" and exit.
 *
 * Build: make cross SRC=ctest_real/test_input.c OUT=ctest_real/test_input.elf
 * Run:   ./bifrost-emu ctest_real/test_input.elf
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

/* Linux input_event layout (AArch64): 24 bytes.
 *   struct input_event {
 *       struct timeval time;  // 16 bytes on AArch64 (8+8)
 *       uint16_t type;
 *       uint16_t code;
 *       int32_t  value;
 *   };
 */
struct input_event {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02

static const char* type_name(uint16_t t) {
    switch (t) {
        case EV_SYN: return "SYN";
        case EV_KEY: return "KEY";
        case EV_REL: return "REL";
        default:     return "???";
    }
}

int main(void) {
    int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/input/event0");
        return 1;
    }
    printf("test_input: opened /dev/input/event0 (fd=%d)\n", fd);
    printf("test_input: waiting for events (will exit after 5)...\n");

    int events_read = 0;
    while (events_read < 5) {
        struct input_event ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) {
            perror("read");
            close(fd);
            return 1;
        }
        if (n == 0) {
            /* No events available (headless mode or no input). */
            printf("test_input: no events (queue empty)\n");
            break;
        }
        if (n != sizeof(ev)) {
            printf("test_input: short read %zd\n", n);
            break;
        }
        printf("test_input: event #%d: type=%s code=0x%04x value=%d\n",
               events_read, type_name(ev.type), ev.code, ev.value);
        events_read++;
    }

    close(fd);
    printf("test_input: done (read %d events)\n", events_read);
    return 0;
}
