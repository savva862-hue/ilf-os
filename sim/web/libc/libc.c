/* Минимальная libc для тестера в браузере: строки, printf через LVGL, память через LVGL. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "math.h"
#include "lvgl.h"

void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }

int vsnprintf(char *b, size_t n, const char *fmt, va_list ap) { return lv_vsnprintf(b, n, fmt, ap); }
int snprintf(char *b, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = lv_vsnprintf(b, n, fmt, ap); va_end(ap); return r;
}
int printf(const char *fmt, ...) { (void)fmt; return 0; }

void *malloc(size_t n) { return lv_malloc(n); }
void *calloc(size_t a, size_t b) { return lv_calloc(a, b); }
void *realloc(void *p, size_t n) { return lv_realloc(p, n); }
void free(void *p) { lv_free(p); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

double sin(double x)
{
    const double pi = 3.14159265358979323846;
    x = x - 2 * pi * __builtin_floor((x + pi) / (2 * pi));   /* в [-pi, pi) */
    double x2 = x * x, t = x, s = x;
    for (int i = 1; i < 10; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
double cos(double x) { return sin(x + 1.57079632679489661923); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
static double exp_(double x)
{
    int k = (int)__builtin_floor(x / 0.6931471805599453);
    double r = x - k * 0.6931471805599453, s = 1, t = 1;
    for (int i = 1; i < 20; i++) { t *= r / i; s += t; }
    while (k > 0) { s *= 2; k--; }
    while (k < 0) { s /= 2; k++; }
    return s;
}
static double log_(double x)
{
    if (x <= 0) return -1e300;
    int k = 0;
    while (x > 2) { x /= 2; k++; }
    while (x < 1) { x *= 2; k--; }
    double y = (x - 1) / (x + 1), y2 = y * y, s = 0, t = y;
    for (int i = 1; i < 40; i += 2) { s += t / i; t *= y2; }
    return 2 * s + k * 0.6931471805599453;
}
double pow(double a, double b) { return a <= 0 ? 0 : exp_(b * log_(a)); }
float powf(float a, float b) { return (float)pow(a, b); }
double log10(double x) { return log_(x) / 2.302585092994046; }
float expf(float x) { return (float)exp_(x); }
float atan2f(float y, float x)
{
    /* грубо, но хватает для интерфейса */
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    float a = (ax > ay ? ay / (ax + 1e-9f) : ax / (ay + 1e-9f));
    float r = a * (0.7853981f + 0.273f * (1 - a));
    if (ay > ax) r = 1.5707963f - r;
    if (x < 0) r = 3.1415926f - r;
    return y < 0 ? -r : r;
}
