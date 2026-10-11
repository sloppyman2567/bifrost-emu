// JPEG decode differential: decompresses a JPEG file to RGB via libjpeg,
// prints dimensions + adler32 of raw pixels.
// Usage: jpegdec_t <file>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

static uint32_t adler(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) { printf("usage: %s file [rawout]\n", argv[0]); return 2; }
    printf("env JSIMD_FORCENEON=%s\n", getenv("JSIMD_FORCENEON") ? getenv("JSIMD_FORCENEON") : "(null)");
    /* input integrity: adler the raw file bytes via fread (stdio path) */
    {
        FILE *g = fopen(argv[1], "rb");
        if (g) {
            uint32_t a = 1, b = 0;
            size_t nn;
            uint8_t chunk[4096];
            while ((nn = fread(chunk, 1, sizeof(chunk), g)) > 0)
                for (size_t k = 0; k < nn; k++) { a = (a + chunk[k]) % 65521; b = (b + a) % 65521; }
            fclose(g);
            printf("input adler=0x%08x\n", (b << 16) | a);
        }
    }
    /* dump raw input copy for host diff */
    {
        FILE *g = fopen(argv[1], "rb");
        FILE *o = fopen("/tmp/jpgin.bin", "wb");
        if (g && o) {
            size_t nn;
            uint8_t chunk[4096];
            while ((nn = fread(chunk, 1, sizeof(chunk), g)) > 0)
                fwrite(chunk, 1, nn, o);
            fclose(o);
        }
        if (g) fclose(g);
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("open fail\n"); return 1; }
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    if (jpeg_read_header(&cinfo, TRUE) != 1) { printf("hdr fail\n"); return 1; }
    jpeg_start_decompress(&cinfo);
    size_t stride = (size_t)cinfo.output_width * (size_t)cinfo.output_components;
    size_t total = stride * (size_t)cinfo.output_height;
    uint8_t *img = malloc(total ? total : 1);
    if (!img) { printf("oom\n"); return 1; }
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *row = img + (size_t)cinfo.output_scanline * stride;
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) break;
    }
    printf("w=%u h=%u comp=%d lines=%u adler=0x%08x outbuf=0x%lx\n",
           cinfo.output_width, cinfo.output_height, cinfo.output_components,
           cinfo.output_scanline, adler(img, total), (unsigned long)img);
    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        if (o) { fwrite(img, 1, total, o); fclose(o); }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    free(img);
    printf("JPEGDEC_OK\n");
    return 0;
}
