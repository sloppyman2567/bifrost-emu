#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Simple LZ77-style compressor (output matches, no actual compression)
typedef struct { uint16_t offset; uint16_t length; uint8_t literal; } Token;

int compress(const uint8_t *in, int len, Token *out) {
    int pos = 0, ntokens = 0;
    while (pos < len) {
        int best_off = 0, best_len = 0;
        int search_start = pos > 4096 ? pos - 4096 : 0;
        for (int off = search_start; off < pos; off++) {
            int match_len = 0;
            while (off + match_len < pos && pos + match_len < len &&
                   in[off + match_len] == in[pos + match_len] && match_len < 255)
                match_len++;
            if (match_len > best_len) { best_len = match_len; best_off = pos - off; }
        }
        if (best_len >= 3) {
            out[ntokens].offset = best_off;
            out[ntokens].length = best_len;
            out[ntokens].literal = 0;
            ntokens++;
            pos += best_len;
        } else {
            out[ntokens].offset = 0;
            out[ntokens].length = 0;
            out[ntokens].literal = in[pos];
            ntokens++;
            pos++;
        }
    }
    return ntokens;
}

int decompress(const Token *tokens, int ntokens, uint8_t *out) {
    int pos = 0;
    for (int i = 0; i < ntokens; i++) {
        if (tokens[i].length > 0) {
            for (int j = 0; j < tokens[i].length; j++) {
                out[pos] = out[pos - tokens[i].offset];
                pos++;
            }
        } else {
            out[pos++] = tokens[i].literal;
        }
    }
    return pos;
}

int main() {
    printf("LZ77 compression test\n");
    const char *text = "the quick brown fox jumps over the lazy dog. the quick brown fox is quick.";
    int len = strlen(text);
    Token tokens[256];
    int ntokens = compress((const uint8_t*)text, len, tokens);
    printf("  Original: %d bytes, %d tokens\n", len, ntokens);
    
    uint8_t decompressed[512];
    int dlen = decompress(tokens, ntokens, decompressed);
    decompressed[dlen] = 0;
    
    printf("  Decompressed: %d bytes\n", dlen);
    printf("  Match: %s\n", dlen == len && memcmp(text, decompressed, len) == 0 ? "OK" : "FAIL");
    
    if (dlen != len || memcmp(text, decompressed, len) != 0) {
        printf("  Original:     \"%s\"\n", text);
        printf("  Decompressed: \"%s\"\n", decompressed);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
