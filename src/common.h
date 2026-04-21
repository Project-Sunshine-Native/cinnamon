#pragma once

#include <stdbool.h>
#ifndef nullptr
#define nullptr NULL
#endif

#if defined(__has_c_attribute)
    #if __has_c_attribute(maybe_unused)
        #define MAYBE_UNUSED [[maybe_unused]]
    #endif
#endif

#ifndef MAYBE_UNUSED
    #if defined(__GNUC__) || defined(__clang__)
        #define MAYBE_UNUSED __attribute__((unused))
    #else
        #define MAYBE_UNUSED
    #endif
#endif