#ifndef BIFROST_GUEST_BIONIC_H
#define BIFROST_GUEST_BIONIC_H
// These resolve to hash-checked AOSP headers, never host libc headers.
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
// Platform linker entrypoint supplied by the pinned libdl_android.so, not an
// NDK API. Each package process already has its own isolated root/namespace.
extern void android_update_LD_LIBRARY_PATH(const char *);
#endif
