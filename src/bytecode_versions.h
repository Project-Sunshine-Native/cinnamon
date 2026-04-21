#pragma once

// To avoid unused code in production builds that are only used for a single game (example: PlayStation 2 target), we can enable/disable specific bytecode versions
//
// If only a single bytecode version is enabled, the checks will be collapsed and removed by the compiler during build time, reducing code size and improving
// performance on low end hardware!

#if defined(ENABLE_BC16)
#  define IS_BC16_OR_HIGHER_ENABLED 1
#else
#  define IS_BC16_OR_HIGHER_ENABLED 0
#endif

#if defined(ENABLE_BC17)
#  define IS_BC17_OR_HIGHER_ENABLED 1
#else
#  define IS_BC17_OR_HIGHER_ENABLED 0
#endif

#if defined(ENABLE_BC16) && defined(ENABLE_BC17)
#  define IS_BC16_OR_BELOW(ctx)   (16 >= ctx->dataWin->gen8.bytecodeVersion)
#  define IS_BC16_OR_HIGHER(ctx)  (ctx->dataWin->gen8.bytecodeVersion >= 16)
#  define IS_BC17_OR_BELOW(ctx)   (17 >= ctx->dataWin->gen8.bytecodeVersion)
#  define IS_BC17_OR_HIGHER(ctx)  (ctx->dataWin->gen8.bytecodeVersion >= 17)
#elif defined(ENABLE_BC16)
#  define IS_BC16_OR_BELOW(ctx)   1
#  define IS_BC16_OR_HIGHER(ctx)  1
#  define IS_BC17_OR_BELOW(ctx)   1
#  define IS_BC17_OR_HIGHER(ctx)  0
#elif defined(ENABLE_BC17)
#  define IS_BC16_OR_BELOW(ctx)   0
#  define IS_BC16_OR_HIGHER(ctx)  1
#  define IS_BC17_OR_BELOW(ctx)   1
#  define IS_BC17_OR_HIGHER(ctx)  1
#else
#  error "You need to build Butterscotch with at least one bytecode version enabled!"
#endif
