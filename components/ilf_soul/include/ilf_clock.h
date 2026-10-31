/*
 * ILF OS — часы: «год прошёл на N%» и русские названия дат.
 * Чистый C, без железа.
 */
#pragma once

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int ilf_is_leap_year(int year);

/* Сколько процентов года прошло (0.0 .. 100.0).
 * year — полный год (2026), yday — день года от 0 (как tm_yday). */
double ilf_year_progress(int year, int yday, int hour, int min, int sec);

/* То же из struct tm (местное время). */
double ilf_year_progress_tm(const struct tm *t);

/* «суббота, 31 октября» */
void ilf_format_date_ru(const struct tm *t, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
