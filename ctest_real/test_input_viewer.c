/* test_input_viewer.c — interactive SDL2 input/fb viewer for bifrost-emu.
 *
 * Opens /dev/fb0 and /dev/input/event0, creates an SDL2 window, and
 * mirrors guest framebuffer changes into the window. Keyboard and mouse
 * events from /dev/input/event0 change the on-screen state, so we can
 * verify the full event path in real time.
 *
 * Controls:
 *   - ESC : exit
 *   - Space: toggle solid color vs bouncing box
 *   - Mouse motion in window: move a dot
 *   - Left click: change palette
 *
 * Build:
 *   make cross SRC=ctest_real/test_input_viewer.c OUT=ctest_real/test_input_viewer.elf
 *
 * Run:
 *   ./bifrost-emu ctest_real/test_input_viewer.elf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <time.h>

struct input_event {
    long long tv_sec;
    long long tv_usec;
    unsigned short type;
    unsigned short code;
    int value;
};

#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_SYN 0x00
#define SYN_REPORT 0x00
#define SYN_DROPPED 0x01

#define KEY_ESC 1
#define KEY_SPACE 57
#define BTN_LEFT 0x110
#define ABS_X 0x00
#define ABS_Y 0x01
#define REL_X 0x00
#define REL_Y 0x01

static uint32_t palette[] = {
    0xFF000000u,
    0xFFFF0000u,
    0xFF00FF00u,
    0xFF0000FFu,
    0xFFFFFF00u,
    0xFF00FFFFu,
    0xFFFF00FFu,
    0xFFFFFFFFu,
};

static void set_pixel(uint32_t* fb, int w, int h, int x, int y, uint32_t color) {
    if (x >= 0 && x < w && y >= 0 && y < h) {
        fb[y * w + x] = color;
    }
}

static void draw_box(uint32_t* fb, int w, int h, int bx, int by, int bw, int bh, uint32_t color) {
    for (int y = by; y < by + bh && y < h; y++) {
        for (int x = bx; x < bx + bw && x < w; x++) {
            set_pixel(fb, w, h, x, y, color);
        }
    }
}

int main(void) {
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO");
        close(fbfd);
        return 1;
    }

    int w = vinfo.xres;
    int h = vinfo.yres;
    size_t fb_size = (size_t)w * h * (vinfo.bits_per_pixel / 8);
    uint32_t* fb = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap");
        close(fbfd);
        return 1;
    }

    int infd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (infd < 0) {
        perror("open /dev/input/event0");
    }

    memset(fb, 0, fb_size);

    int palette_idx = 0;
    int box_x = w / 4;
    int box_y = h / 4;
    int box_dx = 2;
    int box_dy = 2;
    int dot_x = w / 2;
    int dot_y = h / 2;
    int mode = 0; /* 0 = bouncing box, 1 = mouse dot */
    int frame = 0;

    for (;;) {
        if (mode == 0) {
            memset(fb, 0, fb_size);
            draw_box(fb, w, h, box_x, box_y, 40, 40, palette[palette_idx]);
            box_x += box_dx;
            box_y += box_dy;
            if (box_x <= 0 || box_x + 40 >= w) box_dx = -box_dx;
            if (box_y <= 0 || box_y + 40 >= h) box_dy = -box_dy;
        } else {
            memset(fb, 0, fb_size);
            set_pixel(fb, w, h, dot_x, dot_y, palette[palette_idx]);
        }

        if (infd >= 0) {
            struct input_event ev;
            while (read(infd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                    fprintf(stderr, "viewer: SYN_DROPPED\n");
                    continue;
                }
                if (ev.type == EV_KEY) {
                    if (ev.code == KEY_ESC && ev.value == 1) {
                        fprintf(stderr, "viewer: ESC\n");
                        munmap(fb, fb_size);
                        close(fbfd);
                        if (infd >= 0) close(infd);
                        return 0;
                    }
                    if (ev.code == KEY_SPACE && ev.value == 1) {
                        mode = !mode;
                    }
                }
                if (ev.type == EV_KEY && ev.code == BTN_LEFT && ev.value == 1) {
                    palette_idx = (palette_idx + 1) % (sizeof(palette) / sizeof(palette[0]));
                }
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) dot_x += ev.value;
                    if (ev.code == REL_Y) dot_y += ev.value;
                    if (dot_x < 0) dot_x = 0;
                    if (dot_x >= w) dot_x = w - 1;
                    if (dot_y < 0) dot_y = 0;
                    if (dot_y >= h) dot_y = h - 1;
                }
                if (ev.type == EV_ABS) {
                    if (ev.code == ABS_X) dot_x = (ev.value * w) / 32767;
                    if (ev.code == ABS_Y) dot_y = (ev.value * h) / 32767;
                    if (dot_x < 0) dot_x = 0;
                    if (dot_x >= w) dot_x = w - 1;
                    if (dot_y < 0) dot_y = 0;
                    if (dot_y >= h) dot_y = h - 1;
                }
            }
        }

        struct timespec ts = {0, 16 * 1000 * 1000};
        nanosleep(&ts, NULL);
        if (++frame % 120 == 0) {
            fprintf(stderr, "viewer: frame %d mode=%d dot=%d,%d box=%d,%d\n",
                    frame, mode, dot_x, dot_y, box_x, box_y);
        }
    }
}
