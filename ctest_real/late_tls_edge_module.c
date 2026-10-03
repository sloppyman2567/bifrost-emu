// Isolated DSOs for pointer relocation, alignment, and weak TLS regressions.
#if defined(TLS_POINTER)
#ifndef TLS_PADDING
#define TLS_PADDING 16
#endif
#define JOIN_INNER(a, b) a##_##b
#define JOIN(a, b) JOIN_INNER(a, b)
#define SYMBOL(name) JOIN(TLS_NAMESPACE, name)
static int local_value = 0x2468;
int SYMBOL(exported_value) = 0x1357;
__thread char padding[TLS_PADDING];
__thread int *SYMBOL(exported_pointer) = &SYMBOL(exported_value);
__thread int *SYMBOL(local_pointer) = &local_value;
int *SYMBOL(pointer_addr)(void) { return SYMBOL(exported_pointer); }
int *SYMBOL(expected_addr)(void) { return &SYMBOL(exported_value); }
int *SYMBOL(local_pointer_addr)(void) { return SYMBOL(local_pointer); }
int *SYMBOL(local_expected_addr)(void) { return &local_value; }
#elif defined(TLS_ALIGNED)
__thread char aligned_value[64] __attribute__((aligned(64))) = {9};
void *aligned_addr(void) { return aligned_value; }
#elif defined(TLS_WEAK)
extern __thread int optional_tls __attribute__((weak));
int *optional_addr(void) { return &optional_tls; }
#else
#error "Select a TLS edge case"
#endif
