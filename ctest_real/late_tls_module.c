// Shared source for the late-TLS regression modules. Build with the
// LATE_TLS_NAME and LATE_TLS_PADDING defines in scripts/test_late_tls.sh.
#ifndef LATE_TLS_NAME
#error "LATE_TLS_NAME must be defined"
#endif
#ifndef LATE_TLS_PADDING
#define LATE_TLS_PADDING 16
#endif

#define TLS_JOIN_INNER(a, b) a##b
#define TLS_JOIN(a, b) TLS_JOIN_INNER(a, b)
#ifndef LATE_TLS_ACCESSOR
#define LATE_TLS_ACCESSOR TLS_JOIN(LATE_TLS_NAME, _addr)
#endif

__thread char TLS_JOIN(LATE_TLS_NAME, _padding)[LATE_TLS_PADDING];
__thread int TLS_JOIN(LATE_TLS_NAME, _value) = 0x13579bdf;

__attribute__((visibility("default")))
int *LATE_TLS_ACCESSOR(void) {
    return &TLS_JOIN(LATE_TLS_NAME, _value);
}
