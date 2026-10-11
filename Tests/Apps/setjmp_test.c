#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>

jmp_buf buf;

void func3(int depth) {
    if (depth == 0) {
        printf("  longjmp from depth 0\n");
        longjmp(buf, 42);
    }
    printf("  func3 depth %d\n", depth);
    func3(depth - 1);
}

void func2() { func3(3); }
void func1() { func2(); }

int main() {
    printf("setjmp/longjmp test\n");
    
    int r = setjmp(buf);
    if (r == 0) {
        printf("  setjmp returned 0, calling func1\n");
        func1();
        printf("FAIL: should not reach here\n");
        return 1;
    } else {
        printf("  longjmp returned %d\n", r);
        if (r != 42) { printf("FAIL: expected 42\n"); return 1; }
        printf("PASS\n");
        return 0;
    }
}
