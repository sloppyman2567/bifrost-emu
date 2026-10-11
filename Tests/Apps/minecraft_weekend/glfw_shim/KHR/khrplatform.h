#ifndef __khrplatform_h_
#define __khrplatform_h_

#include <stdint.h>
#include <stddef.h>

#define KHRONOS_APICALL
#define KHRONOS_APIENTRY
#define KHRONOS_APIATTRIBUTES

typedef uint8_t khronos_uint8_t;
typedef int8_t khronos_int8_t;
typedef uint16_t khronos_uint16_t;
typedef int16_t khronos_int16_t;
typedef uint32_t khronos_uint32_t;
typedef int32_t khronos_int32_t;
typedef uint64_t khronos_uint64_t;
typedef int64_t khronos_int64_t;

typedef intptr_t khronos_intptr_t;
typedef uintptr_t khronos_uintptr_t;
typedef long khronos_ssize_t;
typedef unsigned long khronos_usize_t;

typedef float khronos_float_t;

#define KHRONOS_FALSE 0
#define KHRONOS_TRUE 1
#define KHRONOS_BOOLEAN_ENUM_FORCE_SIZE 0x7FFFFFFF

#endif
