#pragma once
#define M_PI 3.14159265358979323846
#define fabsf __builtin_fabsf
#define fabs __builtin_fabs
#define sqrtf __builtin_sqrtf
#define sqrt __builtin_sqrt
#define floorf __builtin_floorf
#define floor __builtin_floor
#define ceilf __builtin_ceilf
#define roundf(x) __builtin_floorf((x) + 0.5f)
#define round(x) __builtin_floor((x) + 0.5)
#define fminf __builtin_fminf
#define fmaxf __builtin_fmaxf
float sinf(float x); float cosf(float x);
double sin(double x); double cos(double x);
float powf(float a, float b); double pow(double a, double b);
double log10(double x); float expf(float x);
float atan2f(float y, float x);
