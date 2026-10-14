// Launch the image's actual resource manager without publishing media.player.
// NDK process.h and ResourceManagerService.h, android-15.0.0_r1.
#include <stdbool.h>
#include <stdint.h>
extern bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t);
extern void ABinderProcess_startThreadPool(void);
extern void ABinderProcess_joinThreadPool(void);
extern void instantiate(void) __asm__("_ZN7android22ResourceManagerService11instantiateEv");
int main(void) {
    if (!ABinderProcess_setThreadPoolMaxThreadCount(8)) return 1;
    ABinderProcess_startThreadPool();
    instantiate();
    ABinderProcess_joinThreadPool();
    return 0;
}
