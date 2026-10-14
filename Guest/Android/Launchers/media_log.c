// Realm daemons keep Android's logger API and write to their owned log files.
#include <android/log.h>
__attribute__((constructor)) static void media_logger(void) {
    __android_log_set_logger(__android_log_stderr_logger);
}
