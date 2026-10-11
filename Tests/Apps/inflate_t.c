// Minimal zlib inflate differential: reads a raw zlib stream file,
// uncompress()es it via dlopened libz, prints len + adler32.
// Usage: inflate_t <file>   (reads via open/read, no stdio buffering)
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef unsigned long uLong;
typedef unsigned char Bytef;

static uint32_t adler(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) { printf("usage: %s file [rawout]\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { printf("open fail\n"); return 1; }
    size_t cap = 1 << 20, len = 0;
    uint8_t *in = malloc(cap);
    ssize_t r;
    while ((r = read(fd, in + len, cap - len)) > 0) {
        len += (size_t)r;
        if (len == cap) { cap *= 2; in = realloc(in, cap); }
    }
    close(fd);
    printf("in=%zu\n", len);
    void *h = dlopen("libz.so.1", RTLD_NOW);
    if (!h) { printf("dlopen fail: %s\n", dlerror()); return 1; }
    int (*uc)(Bytef *, uLong *, const Bytef *, uLong) =
        (int (*)(Bytef *, uLong *, const Bytef *, uLong))dlsym(h, "uncompress");
    if (!uc) { printf("dlsym fail\n"); return 1; }
    static uint8_t out[1 << 20];
    uLong outlen = sizeof(out);
    int rc = uc(out, &outlen, in, (uLong)len);
    printf("rc=%d outlen=%lu adler=0x%08x\n", rc, outlen, adler(out, outlen));
    if (argc > 2 && argv[2]) {
        FILE *o = fopen(argv[2], "wb");
        if (o) { fwrite(out, 1, outlen, o); fclose(o); }
    }
    printf("%s\n", (rc == 0 && outlen == 262400) ? "INFLATE_OK" : "INFLATE_BAD");
    return (rc == 0) ? 0 : 1;
}
