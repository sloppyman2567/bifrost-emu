// A guest exit inside nested native BL/BLR calls must unwind callers.
// Falling through after SVC or resuming a caller deliberately traps.
#include <stdio.h>
extern void nested_exit(void);
__asm__(
".text\n.align 4\n.global nested_exit\n.type nested_exit,%function\n"
"nested_exit:\n stp x29,x30,[sp,#-16]!\n bl 1f\n brk #0\n"
"1: stp x29,x30,[sp,#-16]!\n adr x16,2f\n blr x16\n brk #0\n"
"2: mov x0,#0\n mov x8,#94\n svc #0\n brk #0\n"
".size nested_exit,.-nested_exit\n");
int main(void) {
    puts("jit_call_exit: checks passed only if exit status is zero");
    fflush(stdout);
    nested_exit();
    return 1;
}
