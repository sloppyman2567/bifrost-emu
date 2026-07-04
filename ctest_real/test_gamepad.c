/* test_gamepad.c — Test the /dev/input/js0 game controller device.
 *
 * Opens /dev/input/js0, reads js_event records, and prints them.
 * Exits after 10 events or after 2 seconds of no input.
 *
 * Under bifrost-emu with SDL2 + a connected game controller: the SDL2
 * event loop captures game controller events and translates them to
 * Linux js_event records (8 bytes each).
 *
 * Without a game controller (or without SDL2): the device is always
 * empty, so this test will print "no events" and exit.
 *
 * Build: make cross SRC=ctest_real/test_gamepad.c OUT=ctest_real/test_gamepad.elf
 * Run:   ./bifrost-emu ctest_real/test_gamepad.elf
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

/* Linux js_event layout (8 bytes). */
struct js_event {
    uint32_t time;     /* milliseconds since startup */
    int16_t  value;    /* axis value or button value (0/1) */
    uint8_t  type;     /* JS_EVENT_BUTTON=1, JS_EVENT_AXIS=2, INIT=0x80 */
    uint8_t  number;   /* axis/button index */
};

#define JS_EVENT_BUTTON 0x01
#define JS_EVENT_AXIS   0x02
#define JS_EVENT_INIT   0x80

static const char* type_name(uint8_t t) {
    if (t & JS_EVENT_INIT) return "INIT";
    switch (t & ~JS_EVENT_INIT) {
        case JS_EVENT_BUTTON: return "BUTTON";
        case JS_EVENT_AXIS:   return "AXIS";
        default:              return "???";
    }
}

int main(void) {
    int fd = open("/dev/input/js0", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/input/js0");
        return 1;
    }
    printf("test_gamepad: opened /dev/input/js0 (fd=%d)\n", fd);
    printf("test_gamepad: waiting for events (will exit after 10)...\n");

    int events_read = 0;
    while (events_read < 10) {
        struct js_event ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) {
            perror("read");
            close(fd);
            return 1;
        }
        if (n == 0) {
            printf("test_gamepad: no events (queue empty)\n");
            break;
        }
        if (n != sizeof(ev)) {
            printf("test_gamepad: short read %zd\n", n);
            break;
        }
        printf("test_gamepad: event #%d: time=%u type=%s number=%d value=%d\n",
               events_read, ev.time, type_name(ev.type), ev.number, ev.value);
        events_read++;
    }

    close(fd);
    printf("test_gamepad: done (read %d events)\n", events_read);
    return 0;
}
