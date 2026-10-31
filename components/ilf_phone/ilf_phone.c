#include "ilf_phone.h"

#include <stdio.h>
#include <string.h>

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* скопировать строку, не разрезая русскую букву */
static void copy_text(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && (src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* ---------- CTS ---------- */

static int day_of_year(int year, int mon0, int mday)
{
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return cum[mon0] + mday - 1 + (leap && mon0 > 1 ? 1 : 0);
}

bool ilf_cts_parse_time(const uint8_t *d, size_t len, struct tm *t)
{
    if (len < 7) return false;
    int year = get16(d), mon = d[2], day = d[3];
    if (year < 2000 || year > 2200 || mon < 1 || mon > 12 || day < 1 || day > 31) return false;
    if (d[4] > 23 || d[5] > 59 || d[6] > 59) return false;
    memset(t, 0, sizeof *t);
    t->tm_year = year - 1900;
    t->tm_mon = mon - 1;
    t->tm_mday = day;
    t->tm_hour = d[4];
    t->tm_min = d[5];
    t->tm_sec = d[6];
    t->tm_yday = day_of_year(year, mon - 1, day);
    if (len >= 8 && d[7] >= 1 && d[7] <= 7) {
        t->tm_wday = d[7] % 7;   /* CTS: 1 = понедельник … 7 = воскресенье; tm: 0 = воскресенье */
    } else {
        /* посчитать день недели самим (Сакамото) */
        static const int k[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
        int y = year - (mon < 3);
        t->tm_wday = (y + y / 4 - y / 100 + y / 400 + k[mon - 1] + day) % 7;
    }
    t->tm_isdst = -1;
    return true;
}

bool ilf_cts_parse_zone(const uint8_t *d, size_t len, char *tz, size_t tz_len, int *offset_min)
{
    if (len < 2 || (int8_t)d[0] == -128) return false;
    int q = (int8_t)d[0];
    int dst = d[1] == 255 ? 0 : d[1];
    if (q < -48 || q > 56) return false;
    int off = (q + dst) * 15;                    /* минуты к востоку от UTC */
    if (offset_min) *offset_min = off;
    int a = off < 0 ? -off : off;
    int h = a / 60, m = a % 60;
    /* в POSIX TZ знак обратный: Сеул (+9) записывается как "<+09>-9" */
    if (m) snprintf(tz, tz_len, "<%c%02d%02d>%c%d:%02d", off < 0 ? '-' : '+', h, m, off < 0 ? '+' : '-', h, m);
    else   snprintf(tz, tz_len, "<%c%02d>%c%d", off < 0 ? '-' : '+', h, off < 0 ? '+' : '-', h);
    return true;
}

/* ---------- AMS ---------- */

#define AMS_PLAYER 0
#define AMS_TRACK  2
#define AMS_PLAYER_PLAYBACK 1
#define AMS_TRACK_ARTIST 0
#define AMS_TRACK_TITLE  2

size_t ilf_ams_subscribe_track(uint8_t *out)
{
    out[0] = AMS_TRACK; out[1] = AMS_TRACK_ARTIST; out[2] = AMS_TRACK_TITLE;
    return 3;
}

size_t ilf_ams_subscribe_player(uint8_t *out)
{
    out[0] = AMS_PLAYER; out[1] = AMS_PLAYER_PLAYBACK;
    return 2;
}

bool ilf_ams_parse(ilf_ams_state_t *st, const uint8_t *d, size_t len)
{
    if (len < 3) return false;
    const uint8_t *v = d + 3;
    size_t n = len - 3;
    if (d[0] == AMS_TRACK && d[1] == AMS_TRACK_ARTIST) copy_text(st->artist, sizeof st->artist, v, n);
    else if (d[0] == AMS_TRACK && d[1] == AMS_TRACK_TITLE) copy_text(st->title, sizeof st->title, v, n);
    else if (d[0] == AMS_PLAYER && d[1] == AMS_PLAYER_PLAYBACK) st->playing = n > 0 && v[0] == '1';
    else return false;
    st->changed = true;
    return true;
}

/* ---------- ANCS ---------- */

#define ANCS_CMD_GET_ATTR 0
#define ANCS_ATTR_APP     0
#define ANCS_ATTR_TITLE   1
#define ANCS_ATTR_MESSAGE 3

bool ilf_ancs_parse_source(const uint8_t *d, size_t len, ilf_ancs_source_t *o)
{
    if (len < 8 || d[0] > 2) return false;
    o->event = (ilf_ancs_event_t)d[0];
    o->silent = d[1] & 0x01;
    o->preexisting = d[1] & 0x04;
    o->category = d[2];
    o->uid = get32(d + 4);
    return true;
}

size_t ilf_ancs_get_attributes(uint32_t uid, uint8_t *o, size_t cap)
{
    if (cap < 12) return 0;
    o[0] = ANCS_CMD_GET_ATTR;
    o[1] = (uint8_t)uid; o[2] = (uint8_t)(uid >> 8); o[3] = (uint8_t)(uid >> 16); o[4] = (uint8_t)(uid >> 24);
    o[5] = ANCS_ATTR_APP;
    o[6] = ANCS_ATTR_TITLE;   o[7] = ILF_PHONE_TEXT - 1;  o[8] = 0;
    o[9] = ANCS_ATTR_MESSAGE; o[10] = 127;               o[11] = 0;
    return 12;
}

size_t ilf_ancs_action(uint32_t uid, bool positive, uint8_t *o)
{
    o[0] = 2;   /* Perform Notification Action */
    o[1] = (uint8_t)uid; o[2] = (uint8_t)(uid >> 8); o[3] = (uint8_t)(uid >> 16); o[4] = (uint8_t)(uid >> 24);
    o[5] = positive ? 0 : 1;
    return 6;
}

void ilf_ancs_rx_reset(ilf_ancs_rx_t *rx) { rx->len = 0; }

bool ilf_ancs_rx_feed(ilf_ancs_rx_t *rx, const uint8_t *d, size_t len, ilf_ancs_note_t *out)
{
    if (rx->len + len > sizeof rx->buf) rx->len = 0;      /* мусор — начать заново */
    memcpy(rx->buf + rx->len, d, len);
    rx->len += len;
    if (rx->len < 5) return false;
    if (rx->buf[0] != ANCS_CMD_GET_ATTR) { rx->len = 0; return false; }

    ilf_ancs_note_t n;
    memset(&n, 0, sizeof n);
    n.uid = get32(rx->buf + 1);
    size_t p = 5;
    int got = 0;
    while (got < 3) {
        if (p + 3 > rx->len) return false;                 /* ждём следующий кусок */
        uint8_t id = rx->buf[p];
        size_t alen = get16(rx->buf + p + 1);
        if (p + 3 + alen > rx->len) return false;
        const uint8_t *v = rx->buf + p + 3;
        if (id == ANCS_ATTR_APP) copy_text(n.app, sizeof n.app, v, alen);
        else if (id == ANCS_ATTR_TITLE) copy_text(n.title, sizeof n.title, v, alen);
        else if (id == ANCS_ATTR_MESSAGE) copy_text(n.message, sizeof n.message, v, alen);
        p += 3 + alen;
        got++;
    }
    /* хвост, если пришёл, — начало следующего ответа */
    memmove(rx->buf, rx->buf + p, rx->len - p);
    rx->len -= p;
    *out = n;
    return true;
}

const char *ilf_phone_app_name(const char *id)
{
    static const struct { const char *id, *name; } known[] = {
        { "com.apple.MobileSMS", "Сообщения" },
        { "com.apple.mobilephone", "Телефон" },
        { "com.apple.facetime", "FaceTime" },
        { "com.apple.mobilemail", "Почта" },
        { "com.apple.mobilecal", "Календарь" },
        { "com.apple.reminders", "Напоминания" },
        { "ph.telegra.Telegraph", "Telegram" },
        { "net.whatsapp.WhatsApp", "WhatsApp" },
        { "com.kakao.talk", "KakaoTalk" },
        { "com.burbn.instagram", "Instagram" },
        { "com.vk.vkclient", "VK" },
        { "com.zhiliaoapp.musically", "TikTok" },
        { "com.google.ios.youtube", "YouTube" },
        { "com.naver.NaverMap", "Naver Map" },
        { "com.toyopagroup.picaboo", "Snapchat" },
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (strcmp(id, known[i].id) == 0) return known[i].name;
    }
    const char *dot = strrchr(id, '.');
    return dot && dot[1] ? dot + 1 : id;
}
