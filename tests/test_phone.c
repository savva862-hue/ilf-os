/* Тесты разбора iPhone-протоколов: CTS, AMS, ANCS. */
#include "ilf_phone.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, total = 0;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_cts(void)
{
    /* 2026-10-31 21:05:09, суббота (CTS 6) */
    uint8_t d[10] = { 0xEA, 0x07, 10, 31, 21, 5, 9, 6, 0, 0 };
    struct tm t;
    CHECK(ilf_cts_parse_time(d, sizeof d, &t));
    CHECK(t.tm_year == 126 && t.tm_mon == 9 && t.tm_mday == 31);
    CHECK(t.tm_hour == 21 && t.tm_min == 5 && t.tm_sec == 9);
    CHECK(t.tm_wday == 6);
    CHECK(t.tm_yday == 303);
    d[7] = 0;                       /* день недели неизвестен — считаем сами */
    CHECK(ilf_cts_parse_time(d, sizeof d, &t) && t.tm_wday == 6);
    d[7] = 7;                       /* воскресенье */
    CHECK(ilf_cts_parse_time(d, sizeof d, &t) && t.tm_wday == 0);
    d[2] = 13;
    CHECK(!ilf_cts_parse_time(d, sizeof d, &t));
    CHECK(!ilf_cts_parse_time(d, 5, &t));

    char tz[32]; int off = 0;
    uint8_t seoul[2] = { 36, 0 };
    CHECK(ilf_cts_parse_zone(seoul, 2, tz, sizeof tz, &off));
    CHECK(strcmp(tz, "<+09>-9") == 0 && off == 540);
    uint8_t ny[2] = { (uint8_t)-20, 4 };     /* -5 и летнее +1 */
    CHECK(ilf_cts_parse_zone(ny, 2, tz, sizeof tz, &off));
    CHECK(strcmp(tz, "<-04>+4") == 0 && off == -240);
    uint8_t india[2] = { 22, 255 };          /* +5:30, летнее неизвестно */
    CHECK(ilf_cts_parse_zone(india, 2, tz, sizeof tz, &off));
    CHECK(strcmp(tz, "<+0530>-5:30") == 0 && off == 330);
    uint8_t unknown[2] = { 0x80, 0 };
    CHECK(!ilf_cts_parse_zone(unknown, 2, tz, sizeof tz, &off));
}

static void test_ams(void)
{
    uint8_t b[8];
    CHECK(ilf_ams_subscribe_track(b) == 3 && b[0] == 2 && b[1] == 0 && b[2] == 2);
    CHECK(ilf_ams_subscribe_player(b) == 2 && b[0] == 0 && b[1] == 1);

    ilf_ams_state_t st; memset(&st, 0, sizeof st);
    const char *artist = "Кино";
    uint8_t m[80] = { 2, 0, 0 };
    memcpy(m + 3, artist, strlen(artist));
    CHECK(ilf_ams_parse(&st, m, 3 + strlen(artist)));
    CHECK(strcmp(st.artist, "Кино") == 0 && st.changed);

    const char *title = "Звезда по имени Солнце";
    m[1] = 2; memcpy(m + 3, title, strlen(title));
    CHECK(ilf_ams_parse(&st, m, 3 + strlen(title)));
    CHECK(strcmp(st.title, title) == 0);

    /* PlaybackInfo: "1,1.0,12.5" — играет */
    const char *pb = "1,1.0,12.5";
    m[0] = 0; m[1] = 1; memcpy(m + 3, pb, strlen(pb));
    CHECK(ilf_ams_parse(&st, m, 3 + strlen(pb)) && st.playing);
    m[3] = '0';
    CHECK(ilf_ams_parse(&st, m, 3 + strlen(pb)) && !st.playing);

    /* длинное русское название режется по целой букве */
    uint8_t lng[3 + 100] = { 2, 2, 0 };
    for (int i = 0; i < 50; i++) { lng[3 + 2 * i] = 0xD0; lng[4 + 2 * i] = 0xAF; }  /* «Я» ×50 */
    CHECK(ilf_ams_parse(&st, lng, sizeof lng));
    CHECK(strlen(st.title) == 62);
    CHECK(!ilf_ams_parse(&st, m, 2));
}

static size_t attr(uint8_t *p, uint8_t id, const char *s)
{
    size_t n = strlen(s);
    p[0] = id; p[1] = (uint8_t)n; p[2] = (uint8_t)(n >> 8);
    memcpy(p + 3, s, n);
    return 3 + n;
}

static void test_ancs(void)
{
    uint8_t src[8] = { 0, 0x05, 4, 1, 0x78, 0x56, 0x34, 0x12 };
    ilf_ancs_source_t s;
    CHECK(ilf_ancs_parse_source(src, 8, &s));
    CHECK(s.event == ILF_ANCS_ADDED && s.silent && s.preexisting && s.category == 4 && s.uid == 0x12345678);
    src[0] = 3;
    CHECK(!ilf_ancs_parse_source(src, 8, &s));

    uint8_t cmd[16];
    CHECK(ilf_ancs_get_attributes(0x12345678, cmd, sizeof cmd) == 12);
    CHECK(cmd[0] == 0 && cmd[1] == 0x78 && cmd[4] == 0x12 && cmd[5] == 0 && cmd[6] == 1 && cmd[7] == 63 && cmd[9] == 3 && cmd[10] == 127);
    CHECK(ilf_ancs_get_attributes(1, cmd, 5) == 0);
    CHECK(ilf_ancs_action(0x01020304, false, cmd) == 6 && cmd[0] == 2 && cmd[1] == 4 && cmd[4] == 1 && cmd[5] == 1);

    /* ответ: два уведомления подряд, режем на куски по 7 байт */
    uint8_t r[400]; size_t n = 0;
    for (int k = 0; k < 2; k++) {
        r[n++] = 0; r[n++] = (uint8_t)(7 + k); r[n++] = 0; r[n++] = 0; r[n++] = 0;
        n += attr(r + n, 0, "ph.telegra.Telegraph");
        n += attr(r + n, 1, k ? "Мира" : "Мама");
        n += attr(r + n, 3, k ? "пошли гулять" : "Ты поел?");
    }
    ilf_ancs_rx_t rx; ilf_ancs_rx_reset(&rx);
    ilf_ancs_note_t note; int got = 0;
    ilf_ancs_note_t notes[2];
    for (size_t i = 0; i < n; i += 7) {
        size_t c = n - i < 7 ? n - i : 7;
        if (ilf_ancs_rx_feed(&rx, r + i, c, &note)) notes[got++] = note;
        /* второй мог собраться из хвоста — докормить пустым куском */
        if (got == 1 && rx.len && ilf_ancs_rx_feed(&rx, r, 0, &note)) notes[got++] = note;
    }
    CHECK(got == 2);
    CHECK(notes[0].uid == 7 && strcmp(notes[0].title, "Мама") == 0 && strcmp(notes[0].message, "Ты поел?") == 0);
    CHECK(strcmp(notes[0].app, "ph.telegra.Telegraph") == 0);
    CHECK(notes[1].uid == 8 && strcmp(notes[1].title, "Мира") == 0);
    CHECK(rx.len == 0);

    CHECK(strcmp(ilf_phone_app_name("ph.telegra.Telegraph"), "Telegram") == 0);
    CHECK(strcmp(ilf_phone_app_name("com.apple.MobileSMS"), "Сообщения") == 0);
    CHECK(strcmp(ilf_phone_app_name("com.example.Coolapp"), "Coolapp") == 0);
}

int main(void)
{
    test_cts();
    test_ams();
    test_ancs();
    printf("phone: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
