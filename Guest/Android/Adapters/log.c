#include "bionic.h"
#include <android/log.h>

// Formatting stays in guest Bionic, where va_list has its real AAPCS64 ABI.
// Only the final string crosses the existing fixed-argument log-write bridge.

int __android_log_vprint(int priority, const char *tag, const char *format, va_list args) {
    char text[1024];
    if (!format)
        return -22; // EINVAL
    if (vsnprintf(text, sizeof(text), format, args) < 0)
        return -22;
    return __android_log_write(priority, tag, text);
}

int __android_log_print(int priority, const char *tag, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = __android_log_vprint(priority, tag, format, args);
    va_end(args);
    return result;
}

int __android_log_buf_write(int buffer, int priority, const char *tag, const char *text) {
    // There is one stderr sink, rather than Android's per-buffer logd service.
    (void)buffer;
    return __android_log_write(priority, tag, text);
}

int __android_log_buf_print(int buffer, int priority, const char *tag, const char *format, ...) {
    (void)buffer;
    va_list args;
    va_start(args, format);
    int result = __android_log_vprint(priority, tag, format, args);
    va_end(args);
    return result;
}

void __android_log_assert(const char *condition, const char *tag, const char *format, ...) {
    if (format) {
        va_list args;
        va_start(args, format);
        __android_log_vprint(7, tag, format, args);
        va_end(args);
    } else {
        __android_log_print(7, tag, "Assertion failed: %s", condition ? condition : "(unspecified)");
    }
    abort();
}
