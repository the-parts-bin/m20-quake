// Included ahead of every Quake source (Makefile: -include): Quake calls
// the double-precision math functions (sin, cos, sqrt, floor, atan2, ...)
// on floats, which on the m20's single-precision FPU go through software
// doubles. These names map them to the float versions instead.
#ifndef M20_MATH_H
#define M20_MATH_H

#include <math.h>

#define sin(x)      sinf(x)
#define cos(x)      cosf(x)
#define tan(x)      tanf(x)
#define atan(x)     atanf(x)
#define atan2(y, x) atan2f(y, x)
#define sqrt(x)     sqrtf(x)
// (Inline: Quake's values all fit an int. The conversion truncates toward 0.)
static inline float m20_floorf(float x) {
    float f = (float)(int)x;
    return f > x ? f - 1.0f : f;
}
static inline float m20_ceilf(float x) {
    float f = (float)(int)x;
    return f < x ? f + 1.0f : f;
}
#define floor(x)    m20_floorf(x)
#define ceil(x)     m20_ceilf(x)
#define fabs(x)     fabsf(x)
#define pow(x, y)   powf(x, y)

#endif
