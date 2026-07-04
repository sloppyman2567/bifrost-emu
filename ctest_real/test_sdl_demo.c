/* test_sdl_demo.c — minimal SDL2 + framebuffer demo for bifrost-emu.
 *
 * Opens /dev/fb0, draws an animated color cycle pattern, reads keyboard
 * input from /dev/input/event0, and exits on ESC or after 60 frames.
 *
 * This tests:
 *   - /dev/fb0 framebuffer mmap + write
 *   - FBIOGET_VSCREENINFO/FSCREENINFO ioctls
 *   - /dev/input/event0 keyboard input
 *   - Frame animation loop with per-frame sync
 *
 * Build: make cross SRC=ctest_real/test_sdl_demo.c OUT=ctest_real/test_sdl_demo.elf
 * Run:   ./bifrost-emu --fb-dump demo.ppm ctest_real/test_sdl_demo.elf
 *        (or with SDL2 build for live window)
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

/* Linux input event (24 bytes on AArch64). */
struct input_event {
    long long tv_sec;
    long long tv_usec;
    unsigned short type;
    unsigned short code;
    int value;
};

#define EV_KEY 0x01
#define KEY_ESC 1

static void draw_pattern(uint32_t *fb, int w, int h, int frame) {
    /* Draw a moving diagonal color pattern. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r = (x + frame) & 0xFF;
            uint8_t g = (y + frame * 2) & 0xFF;
            uint8_t b = ((x + y + frame * 3) >> 1) & 0xFF;
            /* BGRA format (framebuffer is 32-bit BGRA). */
            fb[y * w + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

int main(void) {
    /* Open framebuffer. */
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) {
        perror("open /dev/fb0");
        return 1;
    }
    printf("fb fd=%d\n", fbfd);

    /* Get screen info. */
    struct fb_var_screeninfo vinfo;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO");
        close(fbfd);
        return 1;
    }
    printf("mode: %ux%u %ubpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    int w = vinfo.xres;
    int h = vinfo.yres;
    size_t fb_size = (size_t)w * h * (vinfo.bits_per_pixel / 8);

    /* mmap the framebuffer. */
    uint32_t *fb = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap");
        close(fbfd);
        return 1;
    }
    printf("fb mapped at %p (%zu bytes)\n", (void*)fb, fb_size);

    /* Open input device (non-blocking). */
    int infd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    printf("input fd=%d\n", infd);

    /* Animation loop: 60 frames. */
    int frame;
    for (frame = 0; frame < 60; frame++) {
        /* Draw the pattern. */
        draw_pattern(fb, w, h, frame * 4);

        /* Check for ESC key. */
        if (infd >= 0) {
            struct input_event ev;
            while (read(infd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == KEY_ESC && ev.value == 1) {
                    printf("ESC pressed, exiting\n");
                    goto done;
                }
            }
        }

        /* Small delay (50ms = ~20fps). */
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);

        if (frame % 10 == 0) {
            printf("frame %d\n", frame);
        }
    }

done:
    printf("drew %d frames\n", frame);
    munmap(fb, fb_size);
    if (infd >= 0) close(infd);
    close(fbfd);
    printf("done\n");
    return 0;
}
