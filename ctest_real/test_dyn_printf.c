// test_dyn_printf.c — minimal printf tests under glibc dynamic.
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main() {
    // Try each printf variant separately to isolate the issue.
    // Use write() as a marker between each test.
    
    write(1, "[1] ", 4);
    printf("a\n");              // simplest possible printf
    fflush(stdout);
    
    write(1, "[2] ", 4);
    printf("hello\n");          // simple string
    fflush(stdout);
    
    write(1, "[3] ", 4);
    printf("%d\n", 42);         // simple integer
    fflush(stdout);
    
    write(1, "[4] ", 4);
    printf("%s\n", "world");    // simple string format
    fflush(stdout);
    
    write(1, "[5] ", 4);
    fprintf(stdout, "fprintf ok\n");
    fflush(stdout);
    
    write(1, "[6] ", 4);
    fputs("fputs ok\n", stdout);
    fflush(stdout);
    
    write(1, "[done]\n", 7);
    return 0;
}
