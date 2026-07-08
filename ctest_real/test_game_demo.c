// test_game_demo.c — a comprehensive game-style demo for bifrost-emu.
// Tests framebuffer rendering, input handling, audio output, and
// game loop timing — all the components needed for a real game.
//
// This is a simple "bouncing ball" demo:
//   - Opens /dev/fb0 for rendering
//   - Draws a bouncing colored ball
//   - Reads keyboard input (ESC to quit)
//   - Generates simple audio tones via /dev/dsp
//   - Runs at ~30fps using nanosleep
//
// Build: make cross SRC=ctest_real/test_game_demo.c OUT=ctest_real/test_game_demo.elf
// Run:   ./bifrost-emu --fb-dump game.ppm ctest_real/test_game_demo.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

struct input_event {
    long long tv_sec;
    long long tv_usec;
    unsigned short type;
    unsigned short code;
    int value;
};

#define FB_W 640
#define FB_H 480
#define BALL_R 30

static uint32_t *fb;
static int fb_fd;

static void put_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < FB_W && y >= 0 && y < FB_H)
        fb[y * FB_W + x] = color;
}

static void fill_rect(int x0, int y0, int w, int h, uint32_t color) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put_pixel(x, y, color);
}

static void draw_ball(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x*x + y*y <= r*r)
                put_pixel(cx + x, cy + y, color);
        }
    }
}

int main() {
    // Open framebuffer
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        write(1, "game: no fb0, using stdout\n", 27);
        // Fallback: just print frames
        for (int i = 0; i < 5; i++) {
            printf("frame %d: ball at (%d,%d)\n", i, 100+i*50, 200);
            struct timespec ts = {0, 33000000}; // 33ms
            nanosleep(&ts, NULL);
        }
        write(1, "game: done\n", 11);
        return 0;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        write(1, "game: ioctl failed\n", 19);
        close(fb_fd);
        return 1;
    }

    size_t fb_size = vinfo.yres_virtual * finfo.line_length;
    fb = mmap(NULL, fb_size, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) {
        write(1, "game: mmap failed\n", 18);
        close(fb_fd);
        return 1;
    }

    // Open input device
    int input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);

    // Open audio device
    int audio_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);

    // Game state
    int ball_x = FB_W / 2;
    int ball_y = FB_H / 2;
    int vx = 5, vy = 3;
    uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF};
    int color_idx = 0;

    write(1, "game: started\n", 14);

    // Game loop — 60 frames
    for (int frame = 0; frame < 60; frame++) {
        // Clear screen (black)
        fill_rect(0, 0, FB_W, FB_H, 0x000000);

        // Update ball position
        ball_x += vx;
        ball_y += vy;
        if (ball_x <= BALL_R || ball_x >= FB_W - BALL_R) {
            vx = -vx;
            color_idx = (color_idx + 1) % 6;
        }
        if (ball_y <= BALL_R || ball_y >= FB_H - BALL_R) {
            vy = -vy;
            color_idx = (color_idx + 1) % 6;
        }

        // Draw ball
        draw_ball(ball_x, ball_y, BALL_R, colors[color_idx]);

        // Draw HUD
        if (frame % 10 == 0) {
            // Could draw text here, but keep it simple
        }

        // Check input (non-blocking)
        if (input_fd >= 0) {
            struct input_event ev;
            while (read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == 1 && ev.code == 1 && ev.value == 1) {
                    // ESC pressed
                    write(1, "game: ESC pressed\n", 18);
                    goto done;
                }
            }
        }

        // Play a short tone on bounce
        if (audio_fd >= 0 && (ball_x <= BALL_R || ball_x >= FB_W - BALL_R ||
                              ball_y <= BALL_R || ball_y >= FB_H - BALL_R)) {
            // Write a few samples of a square wave
            short buf[100];
            for (int i = 0; i < 100; i++)
                buf[i] = (i < 50) ? 8000 : -8000;
            write(audio_fd, buf, sizeof(buf));
        }

        // Frame delay (~33ms = 30fps)
        struct timespec ts = {0, 33000000};
        nanosleep(&ts, NULL);
    }

done:
    write(1, "game: done\n", 11);

    if (input_fd >= 0) close(input_fd);
    if (audio_fd >= 0) close(audio_fd);
    munmap(fb, fb_size);
    close(fb_fd);
    return 0;
}
