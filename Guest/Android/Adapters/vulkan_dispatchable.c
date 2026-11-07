// Dispatchable-object storage and the generated-shim registry.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vulkan_dispatchable.h"

enum { BF_VK_OBJECT_WORDS = 2 };

void *bf_vk_wrap(uint64_t host_handle) {
    uint64_t *object = (uint64_t *)calloc(BF_VK_OBJECT_WORDS, sizeof(uint64_t));
    if (!object) return NULL;
    object[0] = BF_VK_ICD_LOADER_MAGIC;  // the loader overwrites this
    object[1] = host_handle;
    return object;
}

uint64_t bf_vk_unwrap(const void *object) {
    return object ? ((const uint64_t *)object)[1] : 0;
}

void bf_vk_drop(void *object) { free(object); }

// Emitted by build-android-adapters.py from vk.xml: {loader name, host symbol,
// shim} triples, then a zero sentinel. The two names differ for aliases: the
// loader asks for `vkGetPhysicalDeviceFeatures2KHR`, the host exports only
// `vkGetPhysicalDeviceFeatures2`.
struct bf_vk_shim_entry {
    const char *name;
    const char *host_name;
    void (*function)(void);
};
extern const struct bf_vk_shim_entry bf_vk_shims[];

void *bf_vk_shim(const char *name, const char **host_name) {
    if (!name) return NULL;
    for (const struct bf_vk_shim_entry *entry = bf_vk_shims; entry->name; ++entry) {
        if (!strcmp(entry->name, name)) {
            if (host_name) *host_name = entry->host_name;
            return (void *)entry->function;
        }
    }
    return NULL;
}

void bf_vk_missing(const char *name) {
    char line[192];
    int length = snprintf(line, sizeof line,
                          "vulkan.bifrost: host loader does not export %s\n",
                          name ? name : "?");
    if (length > 0) {
        if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
        (void)!write(2, line, (size_t)length);
    }
    _exit(126);
}
