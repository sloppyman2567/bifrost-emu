// Guest-side dispatchable objects for the Vulkan HAL module.
//
// Android's libvulkan loader owns the first word of every dispatchable object a
// driver returns: it validates ICD_LOADER_MAGIC there and then stores its own
// dispatch table in it (loader_set_dispatch). A host Vulkan handle cannot serve
// as that object, because the guest cannot read or write host memory — handing
// one over made the loader fault on an unmapped host page.
//
// So every handle this module returns is a two-word guest object:
//
//   [0] loader dispatch slot (starts as BF_VK_ICD_LOADER_MAGIC)
//   [1] the host handle the DisplayThunk calls take
//
// Commands whose first argument is dispatchable are handled by generated shims
// (Tools/Android/build-android-adapters.py) that swap the object for word 1
// before the thunk marshals the call. The functions here cover the commands
// that create, destroy or carry handles, where a shim cannot decide alone.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan_core.h>

// vk_icd.h: ICD_LOADER_MAGIC. The loader replaces it with its dispatch table.
#define BF_VK_ICD_LOADER_MAGIC 0x01CDC0DEull

void *bf_vk_wrap(uint64_t host_handle);
uint64_t bf_vk_unwrap(const void *object);
void bf_vk_drop(void *object);

// A dispatchable object's host handle, typed for the host call.
#define HOST_HANDLE(type, object) ((type)(uintptr_t)bf_vk_unwrap(object))

// Command name -> generated unwrapping shim, or NULL when the registry has no
// such command. `host_name` receives the symbol to resolve in the host loader:
// a KHR alias resolves through its core target, which is the name the Linux
// loader actually exports.
void *bf_vk_shim(const char *name, const char **host_name);

// Never returns. A shim was entered for a name the host loader does not export.
__attribute__((noreturn)) void bf_vk_missing(const char *name);

// Commands each device extension adds, generated from vk.xml by
// build-android-adapters.py. A driver that advertises an extension owes the
// app every command in it: where it cannot answer, the loader installs a NULL
// dispatch slot and an app that enabled the extension calls it.
struct bf_vk_extension {
    const char *name;
    const char *const *commands;  // NULL-terminated
    uint32_t command_count;
};
extern const struct bf_vk_extension bf_vk_extensions[];  // NULL name sentinel

// 1 when every command of `extension_name` has a shim and a host symbol.
// Extensions the registry does not know are left to the host's own answers.
int bf_vk_extension_servable(const char *extension_name);

// Command name -> the handle-creating/destroying entry points of
// vulkan_handles.c, or NULL.
PFN_vkVoidFunction vulkan_handles_symbol(const char *name);