#pragma once
#include <stddef.h>
#include <stdarg.h>
typedef struct FILE FILE;
int snprintf(char *b, size_t n, const char *fmt, ...);
int vsnprintf(char *b, size_t n, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
