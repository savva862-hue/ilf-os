#include "ilf_clock.h"

#include <stdio.h>

int ilf_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

double ilf_year_progress(int year, int yday, int hour, int min, int sec)
{
    const double days = ilf_is_leap_year(year) ? 366.0 : 365.0;
    const double elapsed = (double)yday * 86400.0 + hour * 3600.0 + min * 60.0 + sec;
    double p = elapsed / (days * 86400.0) * 100.0;
    if (p < 0.0) p = 0.0;
    if (p > 100.0) p = 100.0;
    return p;
}

double ilf_year_progress_tm(const struct tm *t)
{
    return ilf_year_progress(t->tm_year + 1900, t->tm_yday, t->tm_hour, t->tm_min, t->tm_sec);
}

void ilf_format_date_ru(const struct tm *t, char *out, size_t out_len)
{
    static const char *wdays[7] = {
        "воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота",
    };
    static const char *months[12] = {
        "января", "февраля", "марта", "апреля", "мая", "июня",
        "июля", "августа", "сентября", "октября", "ноября", "декабря",
    };
    int wd = (t->tm_wday >= 0 && t->tm_wday < 7) ? t->tm_wday : 0;
    int mo = (t->tm_mon >= 0 && t->tm_mon < 12) ? t->tm_mon : 0;
    snprintf(out, out_len, "%s, %d %s", wdays[wd], t->tm_mday, months[mo]);
}
