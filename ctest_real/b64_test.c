#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_encode(const unsigned char *in, int len, char *out) {
    int i, j;
    for (i = 0, j = 0; i < len;) {
        uint32_t octet_a = i < len ? in[i++] : 0;
        uint32_t octet_b = i < len ? in[i++] : 0;
        uint32_t octet_c = i < len ? in[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64chars[(triple >> 18) & 0x3F];
        out[j++] = b64chars[(triple >> 12) & 0x3F];
        out[j++] = b64chars[(triple >> 6) & 0x3F];
        out[j++] = b64chars[triple & 0x3F];
    }
    int pad = (3 - len % 3) % 3;
    for (i = 0; i < pad; i++) out[j - 1 - i] = '=';
    out[j] = 0;
    return j;
}

int b64_decode_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, int len, unsigned char *out) {
    int i, j;
    for (i = 0, j = 0; i < len;) {
        int a = b64_decode_val(in[i++]);
        int b = b64_decode_val(in[i++]);
        int c = b64_decode_val(in[i++]);
        int d = b64_decode_val(in[i++]);
        if (a < 0 || b < 0) break;
        out[j++] = (a << 2) | (b >> 4);
        if (c >= 0) out[j++] = ((b & 15) << 4) | (c >> 2);
        if (d >= 0) out[j++] = ((c & 3) << 6) | d;
    }
    return j;
}

int main() {
    printf("Base64 roundtrip test\n");
    const char *tests[] = {"", "f", "fo", "foo", "Hello World!", "bifrost-emu rocks", NULL};
    for (int i = 0; tests[i]; i++) {
        char encoded[256];
        unsigned char decoded[256];
        int elen = b64_encode((const unsigned char*)tests[i], strlen(tests[i]), encoded);
        int dlen = b64_decode(encoded, strlen(encoded), decoded);
        decoded[dlen] = 0;
        printf("  \"%s\" -> \"%s\" -> \"%s\" %s\n", tests[i], encoded, decoded,
               strcmp(tests[i], (char*)decoded) == 0 ? "OK" : "FAIL");
        if (strcmp(tests[i], (char*)decoded) != 0) return 1;
    }
    printf("PASS\n");
    return 0;
}
