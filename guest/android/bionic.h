#ifndef BIFROST_GUEST_BIONIC_H
#define BIFROST_GUEST_BIONIC_H
// Freestanding declarations for the pinned ARM64 Bionic adapter build.
// No host libc headers or startup objects are used.
#include <stddef.h>
#include <stdint.h>
extern int printf(const char *, ...);
extern int fflush(void *);
extern int snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, size_t);
extern int memcmp(const void *, const void *, size_t);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern int dlclose(void *);
extern int open(const char *, int, ...);
extern int close(int);
extern long read(int, void *, size_t);
extern long write(int, const void *, size_t);
extern long lseek(int, long, int);
extern long syscall(long, ...);
extern void *mmap(void *, size_t, int, int, int, long);
extern int munmap(void *, size_t);
extern int ioctl(int, unsigned long, ...);
extern int usleep(unsigned int);
extern void _exit(int) __attribute__((noreturn));
#endif
