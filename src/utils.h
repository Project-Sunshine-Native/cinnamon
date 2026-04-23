#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define nullptr NULL

#define forEach(type, item, array, count) \
    for (typeof(count) item##_i_ = 0; item##_i_ < (count); item##_i_++) \
    for (type* item = &(array)[item##_i_]; item; item = NULL)

#define forEachIndexed(type, item, index, array, count) \
    for (typeof(count) index = 0; index < (count); index++) \
    for (type* item = &(array)[index]; item; item = NULL)

// The "typeof((typeof(n))0" is used to remove the "const" from the typeof

#define repeat(n, it) for (typeof((typeof(n))0) it = 0; it < (n); it++)

#define require(condition) \
    do { \
        if (!(condition)) { \
        fprintf(stderr, "Requirement failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#define requireMessage(condition, message) \
do { \
if (!(condition)) { \
fprintf(stderr, "Requirement failed at %s:%d: %s\n", __FILE__, __LINE__, message); \
abort(); \
} \
} while (0)

#define requireNotNull(ptr) ({ \
typeof(ptr) _val = (ptr); \
if (_val == NULL) { \
fprintf(stderr, "%s:%d: requireNotNull failed: '%s'\n", __FILE__, __LINE__, #ptr); \
abort(); \
} \
_val; \
})

#define requireNotNullMessage(ptr, msg) ({ \
typeof(ptr) _val = (ptr); \
if (_val == NULL) { \
fprintf(stderr, "%s:%d: requireNotNull failed: %s\n", __FILE__, __LINE__, (msg)); \
abort(); \
} \
_val; \
})

static size_t gHeapUsed = 0;

// todo: finish this and make gHeapUsed track all allocations and not just malloc and calloc
static void logMemUse(const char* tag)
{
    printf("[MEM] %-40s tracked used: %lu KB\n",
        tag ? tag : "(null)",
        (unsigned long)(gHeapUsed / 1024));
}

#ifdef FULL_MEM_LOG
  #define LOG_MEM_USE(action) logMemUse(action)
#else
  #define LOG_MEM_USE(action) ((void)0)
#endif

// Safe allocation macros - check for nullptr and abort with file/line info
#define safeMalloc(size) ({ \
    void* _ptr = malloc(size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: malloc(%zu) failed at %s:%d\n", (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    gHeapUsed += size; \
    _ptr; \
})

#define safeCalloc(count, size) ({ \
    void* _ptr = calloc(count, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: calloc(%zu, %zu) failed at %s:%d\n", (size_t)(count), (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    gHeapUsed += (size_t)(count) * (size_t)(size); \
    _ptr; \
})

#define safeRealloc(ptr, size) ({ \
    size_t oldSize = sizeof(ptr); \
    void* _ptr = realloc(ptr, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: realloc(%zu) failed at %s:%d\n", (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeMemalign(alignment, size) ({ \
    void* _ptr = memalign(alignment, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: memalign(%zu, %zu) failed at %s:%d\n", (size_t)(alignment), (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeStrdup(str) ({ \
    char* _ptr = strdup(str); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: strdup() failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeFree(ptr) do { \
    if (ptr) { \
        free(ptr); \
        ptr = NULL; \
        gHeapUsed -= sizeof(ptr); \
    } \
} while (0)

// Truncates to 6 decimal places, matching the HTML5 runner's ClampFloat
static inline double clampFloat(double f) {
    return ((double) ((int64_t) (f * 1000000.0))) / 1000000.0;
}

#define BGR_B(c) (((c) >> 16) & 0xFF)
#define BGR_G(c) (((c) >>  8) & 0xFF)
#define BGR_R(c) (((c) >>  0) & 0xFF)

#define shcopyFromTo(src, dst)                        \
do {                                        \
(dst) = NULL;                           \
for (int i = 0; i < shlen(src); i++)    \
shput((dst), (src)[i].key, (src)[i].value); \
} while (0)

typedef struct {
    char* key;
    bool value;
} StringBooleanEntry;