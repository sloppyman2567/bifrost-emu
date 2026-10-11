// test_strchrnul.c — test SIMD strchrnul with various inputs.
#include <string.h>
#include <stdio.h>
#include <unistd.h>

int main() {
    // Test 1: find a character in a short string
    const char *s = "hello.world";
    char *p = strchrnul(s, '.');
    write(1, "[1 strchrnul '.'] ", 18);
    if (p && *p == '.') {
        write(1, "FOUND at ", 9);
        char buf[4]; buf[0] = '0' + (p - s); buf[1] = '\n'; buf[2] = 0;
        write(1, buf, 2);
    } else if (p && *p == 0) {
        write(1, "NOT FOUND (NUL)\n", 16);
    } else {
        write(1, "FAIL\n", 5);
    }

    // Test 2: find character not in string
    p = strchrnul(s, 'z');
    write(1, "[2 strchrnul 'z'] ", 18);
    if (*p == 0) write(1, "NOT FOUND (ok)\n", 15);
    else write(1, "FAIL\n", 5);

    // Test 3: find first character
    p = strchrnul(s, 'h');
    write(1, "[3 strchrnul 'h'] ", 18);
    if (p == s) write(1, "FOUND at 0\n", 11);
    else write(1, "FAIL\n", 5);

    // Test 4: find last character
    p = strchrnul(s, 'd');
    write(1, "[4 strchrnul 'd'] ", 18);
    if (p && *p == 'd' && *(p+1) == 0) write(1, "FOUND at end\n", 13);
    else write(1, "FAIL\n", 5);

    // Test 5: long string (>16 chars to exercise SIMD loop)
    const char *long_s = "0123456789abcdef.XYZ";
    p = strchrnul(long_s, '.');
    write(1, "[5 long strchrnul] ", 19);
    if (p && *p == '.' && (p - long_s) == 16) write(1, "FOUND at 16\n", 12);
    else write(1, "FAIL\n", 5);

    // Test 6: strchrnul on a string with the char at position 16+ (needs the loop)
    const char *long_s2 = "0123456789abcdefXYZ.1";
    p = strchrnul(long_s2, '.');
    write(1, "[6 strchrnul pos 19] ", 21);
    if (p && *p == '.' && (p - long_s2) == 19) write(1, "FOUND at 19\n", 12);
    else { write(1, "FAIL p-s=", 9); char b[8]; b[0]='0'+(p-long_s2)/10; b[1]='0'+(p-long_s2)%10; b[2]='\n'; write(1,b,3); }

    // Test 7: printf with %.2f — the original failing case
    write(1, "[7 %.2f] ", 9);
    printf("%.2f\n", 3.14);
    
    // Test 8: check if the issue is in the output buffer
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%f", 3.14);
    write(1, "[8 snprintf] len=", 17);
    char lb[8]; lb[0]='0'+n; lb[1]='\n'; write(1,lb,2);
    write(1, buf, n);
    write(1, "\n", 1);
    
    // Print the raw bytes
    write(1, "[9 raw bytes] ", 14);
    for (int i = 0; i < n; i++) {
        char hex[4];
        hex[0] = "0123456789abcdef"[(buf[i] >> 4) & 0xf];
        hex[1] = "0123456789abcdef"[buf[i] & 0xf];
        hex[2] = ' ';
        write(1, hex, 3);
    }
    write(1, "\n", 1);

    write(1, "[done]\n", 7);
    return 0;
}
