/* test_fb.c — Test the /dev/fb0 virtual framebuffer.
 *
 * Opens /dev/fb0, queries the mode via FBIOGET_VSCREENINFO, mmaps the
 * framebuffer, writes a gradient pattern, and exits. Run with:
 *
 *   bifrost-emu --fb-dump out.ppm test_fb.elf
 *
 * Then view out.ppm in any image viewer (or `convert out.ppm out.png`
 * via ImageMagick).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* Linux framebuffer ioctl numbers and structs (minimal subset). */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, reserved[6];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open /dev/fb0"); return 1; }
    printf("fd=%d\n", fd);

    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0) {
        perror("FBIOGET_VSCREENINFO"); return 2;
    }
    printf("mode: %ux%u, %u bpp\n", v.xres, v.yres, v.bits_per_pixel);
    printf("  red:   off=%u len=%u\n", v.red.offset,   v.red.length);
    printf("  green: off=%u len=%u\n", v.green.offset, v.green.length);
    printf("  blue:  off=%u len=%u\n", v.blue.offset,  v.blue.length);

    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) {
        perror("FBIOGET_FSCREENINFO"); return 3;
    }
    printf("fix: id='%s' smem_len=%u line_length=%u\n",
           f.id, f.smem_len, f.line_length);

    size_t fb_size = (size_t)f.smem_len;
    if (fb_size == 0) fb_size = (size_t)v.xres * v.yres * (v.bits_per_pixel / 8);
    printf("mmapping %zu bytes\n", fb_size);

    uint8_t *fb = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 4; }
    printf("fb mapped at %p\n", fb);

    /* Draw a horizontal gradient: red on the left, green in the middle,
     * blue on the right. Each pixel is 32-bit BGRA. */
    for (uint32_t y = 0; y < v.yres; y++) {
        for (uint32_t x = 0; x < v.xres; x++) {
            uint8_t *px = fb + (size_t)y * f.line_length + (size_t)x * 4;
            px[0] = (uint8_t)(x * 255 / v.xres);          /* B */
            px[1] = (uint8_t)((v.xres - x) * 255 / v.xres); /* G */
            px[2] = (uint8_t)(y * 255 / v.yres);          /* R */
            px[3] = 255;                                  /* A */
        }
    }
    printf("gradient drawn\n");

    munmap(fb, fb_size);
    close(fd);
    printf("done\n");
    return 0;
}
