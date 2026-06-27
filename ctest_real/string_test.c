#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

int main() {
    printf("String manipulation test\n");
    
    // strlen
    const char *s = "Hello, bifrost-emu!";
    if (strlen(s) != 19) { printf("FAIL: strlen\n"); return 1; }
    printf("  strlen: OK\n");
    
    // strcmp
    if (strcmp("abc", "abc") != 0) { printf("FAIL: strcmp equal\n"); return 1; }
    if (strcmp("abc", "abd") >= 0) { printf("FAIL: strcmp less\n"); return 1; }
    printf("  strcmp: OK\n");
    
    // strchr
    if (strchr(s, 'b') != s + 7) { printf("FAIL: strchr\n"); return 1; }
    printf("  strchr: OK\n");
    
    // strstr
    if (strstr(s, "bifrost") != s + 7) { printf("FAIL: strstr\n"); return 1; }
    printf("  strstr: OK\n");
    
    // memcpy
    char buf[32];
    memcpy(buf, s, 20);
    if (strcmp(buf, s) != 0) { printf("FAIL: memcpy\n"); return 1; }
    printf("  memcpy: OK\n");
    
    // tolower/toupper
    char upper[32];
    for (int i = 0; s[i]; i++) upper[i] = toupper(s[i]);
    upper[19] = 0;
    if (strcmp(upper, "HELLO, BIFROST-EMU!") != 0) { printf("FAIL: toupper got '%s'\n", upper); return 1; }
    printf("  toupper: OK\n");
    
    // strtok
    char str[] = "a,b,,c";
    char *tok = strtok(str, ",");
    if (!tok || strcmp(tok, "a") != 0) { printf("FAIL: strtok 1\n"); return 1; }
    tok = strtok(NULL, ",");
    if (!tok || strcmp(tok, "b") != 0) { printf("FAIL: strtok 2\n"); return 1; }
    tok = strtok(NULL, ",");
    if (!tok || strcmp(tok, "c") != 0) { printf("FAIL: strtok 3\n"); return 1; }
    tok = strtok(NULL, ",");
    if (tok) { printf("FAIL: strtok 4\n"); return 1; }
    printf("  strtok: OK\n");
    
    // sprintf
    char fmt[64];
    sprintf(fmt, "int=%d hex=0x%x str=%s", 42, 255, "test");
    if (strcmp(fmt, "int=42 hex=0xff str=test") != 0) { printf("FAIL: sprintf got '%s'\n", fmt); return 1; }
    printf("  sprintf: OK\n");
    
    // atoi
    if (atoi("12345") != 12345) { printf("FAIL: atoi\n"); return 1; }
    if (atoi("-42") != -42) { printf("FAIL: atoi neg\n"); return 1; }
    printf("  atoi: OK\n");
    
    printf("PASS\n");
    return 0;
}
