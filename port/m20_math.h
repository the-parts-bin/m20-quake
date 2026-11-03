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
#define floor(x)    floorf(x)
#define ceil(x)     ceilf(x)
#define fabs(x)     fabsf(x)
#define pow(x, y)   powf(x, y)

#endif
