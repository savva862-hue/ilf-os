#pragma once
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
