#pragma once

#include "common.h"
#include <math.h>
#include <stdlib.h>

#ifdef USE_FLOAT_REALS

typedef float GMLReal;

#define GMLReal_sin sinf
#define GMLReal_cos cosf
#define GMLReal_asin asinf
#define GMLReal_atan2 atan2f
#define GMLReal_sqrt sqrtf
#define GMLReal_fabs fabsf
#define GMLReal_fmod fmodf
#define GMLReal_floor floorf
#define GMLReal_ceil ceilf
#define GMLReal_round roundf
#define GMLReal_pow powf
#define GMLReal_fmax fmaxf
#define GMLReal_fmin fminf
#define GMLReal_strtod(str, endptr) strtof(str, endptr)

#else

typedef double GMLReal;

#define GMLReal_sin sin
#define GMLReal_cos cos
#define GMLReal_asin asin
#define GMLReal_atan2 atan2
#define GMLReal_sqrt sqrt
#define GMLReal_fabs fabs
#define GMLReal_fmod fmod
#define GMLReal_floor floor
#define GMLReal_ceil ceil
#define GMLReal_round round
#define GMLReal_pow pow
#define GMLReal_fmax fmax
#define GMLReal_fmin fmin
#define GMLReal_strtod(str, endptr) strtod(str, endptr)

#endif
