/* Тесты души и часов. Запуск: cmake -S tests -B build/tests && cmake --build build/tests && ./build/tests/ilf_tests */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ilf_clock.h"
#include "ilf_soul.h"
#include "ilf_lock.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void run_until(ilf_soul_t *s, uint32_t *now, uint32_t until)
{
    while (*now < until) { *now += 33; ilf_soul_update(s, *now); }
}

static void test_year_progress(void)
{
    CHECK(fabs(ilf_year_progress(2026, 0, 0, 0, 0) - 0.0) < 1e-9);
    CHECK(fabs(ilf_year_progress(2026, 364, 23, 59, 59) - 100.0) < 0.001);
    /* 2 июля 12:00 в невисокосном году — ровно половина */
    CHECK(fabs(ilf_year_progress(2026, 182, 12, 0, 0) - 50.0) < 1e-9);
    /* 31 октября 2026 = день 303 → 83% */
    CHECK((int)ilf_year_progress(2026, 303, 0, 0, 0) == 83);
    CHECK(ilf_is_leap_year(2028) && !ilf_is_leap_year(2100) && ilf_is_leap_year(2000));
    CHECK(fabs(ilf_year_progress(2028, 183, 0, 0, 0) - 50.0) < 1e-9);
}

static void test_date_ru(void)
{
    struct tm t = {0};
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 31; t.tm_wday = 6;
    char buf[64];
    ilf_format_date_ru(&t, buf, sizeof buf);
    CHECK(strcmp(buf, "суббота, 31 октября") == 0);
}

static void test_blinks(void)
{
    ilf_soul_t s; uint32_t now = 1000;
    ilf_soul_init(&s, 42, now);
    int closed_frames = 0;
    for (int i = 0; i < 400; i++) { /* ~13 секунд */
        now += 33; ilf_soul_update(&s, now);
        if (ilf_soul_face(&s)->eyelid > 0.5f) closed_frames++;
    }
    CHECK(closed_frames > 0);        /* моргнул хотя бы раз */
    CHECK(closed_frames < 40);       /* но глаза в основном открыты */
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_CALM);
}

static void test_shake_scares_then_calms(void)
{
    ilf_soul_t s; uint32_t now = 1000;
    ilf_soul_init(&s, 1, now);
    ilf_event_t ev = { .type = ILF_EV_SHAKE };
    ilf_soul_handle(&s, &ev, now);
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_SCARED);
    CHECK(ilf_soul_face(&s)->mouth_open);
    run_until(&s, &now, 1000 + ILF_SOUL_SCARED_MS + 100);
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_CALM);
}

static void test_many_shakes_make_sad(void)
{
    ilf_soul_t s; uint32_t now = 1000;
    ilf_soul_init(&s, 1, now);
    ilf_event_t ev = { .type = ILF_EV_SHAKE };
    for (int i = 0; i < 3; i++) { ilf_soul_handle(&s, &ev, now); now += 1000; }
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_SAD);
    CHECK(ilf_soul_face(&s)->mouth < 0);
    /* редкие встряски не обижают */
    ilf_soul_init(&s, 1, now);
    for (int i = 0; i < 3; i++) { ilf_soul_handle(&s, &ev, now); now += 20000; }
    CHECK(ilf_soul_face(&s)->emotion != ILF_EMO_SAD);
}

static void test_touch_looks_and_smiles(void)
{
    ilf_soul_t s; uint32_t now = 1000;
    ilf_soul_init(&s, 7, now);
    ilf_event_t ev = { .type = ILF_EV_TOUCH, .touch = { .x = 360, .y = 224 } };
    ilf_soul_handle(&s, &ev, now);
    run_until(&s, &now, now + 500);
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_HAPPY);
    CHECK(ilf_soul_face(&s)->mouth > 0.9f);
    CHECK(ilf_soul_face(&s)->gaze_x > 0.7f); /* смотрит вправо, куда ткнули */
}

static void test_falls_asleep_and_wakes(void)
{
    ilf_soul_t s; uint32_t now = 0;
    ilf_soul_init(&s, 3, now);
    run_until(&s, &now, ILF_SOUL_SLEEPY_AFTER_MS + 100);
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_SLEEPY);
    run_until(&s, &now, ILF_SOUL_ASLEEP_AFTER_MS + 100);
    CHECK(ilf_soul_is_asleep(&s));
    CHECK(ilf_soul_face(&s)->eyelid > 0.99f);
    ilf_event_t ev = { .type = ILF_EV_BUTTON };
    ilf_soul_handle(&s, &ev, now);
    CHECK(!ilf_soul_is_asleep(&s));
}

static void test_hungry_on_low_battery(void)
{
    ilf_soul_t s; uint32_t now = 0;
    ilf_soul_init(&s, 3, now);
    ilf_event_t ev = { .type = ILF_EV_BATTERY, .battery = { .percent = 10, .charging = false } };
    ilf_soul_handle(&s, &ev, now);
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_HUNGRY);
    ev.battery.charging = true;
    ilf_soul_handle(&s, &ev, now + 10);
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_CALM);
}

static void test_friend_message_happy(void)
{
    ilf_soul_t s; uint32_t now = 0;
    ilf_soul_init(&s, 3, now);
    run_until(&s, &now, ILF_SOUL_ASLEEP_AFTER_MS + 100);
    ilf_event_t ev = { .type = ILF_EV_FRIEND_MESSAGE };
    ilf_soul_handle(&s, &ev, now);
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_HAPPY);
}

static void test_time_wraparound(void)
{
    ilf_soul_t s; uint32_t now = 0xFFFFF000u;
    ilf_soul_init(&s, 9, now);
    ilf_event_t ev = { .type = ILF_EV_SHAKE };
    ilf_soul_handle(&s, &ev, now);
    for (int i = 0; i < 100; i++) { now += 33; ilf_soul_update(&s, now); } /* переполнение uint32 */
    CHECK(ilf_soul_face(&s)->emotion == ILF_EMO_CALM);
}

/* игрушечный хеш для тестов замка (на плате — mbedtls SHA-256) */
static void toy_sha(const uint8_t *d, size_t n, uint8_t out[32])
{
    for (int k = 0; k < 4; k++) {
        uint64_t x = 1469598103934665603ULL + (uint64_t)k;
        for (size_t i = 0; i < n; i++) { x ^= d[i]; x *= 1099511628211ULL; }
        memcpy(out + k * 8, &x, 8);
    }
}

static void test_lock(void)
{
    uint8_t salt[16] = { 1, 2, 3 };
    ilf_pin_t pin;
    ilf_lock_make_pin(&pin, "2580", salt, toy_sha);
    ilf_lock_t l;
    ilf_lock_init(&l, &pin);
    uint32_t now = 1000;
    CHECK(ilf_lock_is_locked(&l, now));
    CHECK(ilf_lock_try(&l, "1111", now, toy_sha) == ILF_LOCK_WRONG);
    CHECK(ilf_lock_try(&l, "25a0", now, toy_sha) == ILF_LOCK_WRONG);
    CHECK(ilf_lock_try(&l, "2580", now, toy_sha) == ILF_LOCK_OK);
    CHECK(!ilf_lock_is_locked(&l, now));
    /* экран погас ненадолго — остаётся открытым */
    ilf_lock_screen_off(&l, now);
    now += 60000;
    ilf_lock_screen_on(&l, now);
    CHECK(!ilf_lock_is_locked(&l, now));
    /* погас надолго — закрылся */
    ilf_lock_screen_off(&l, now);
    now += ILF_LOCK_GRACE_MS + 1;
    ilf_lock_screen_on(&l, now);
    CHECK(ilf_lock_is_locked(&l, now));
    /* пять ошибок — пауза, даже верный PIN не принимается */
    for (int i = 0; i < ILF_LOCK_TRIES - 1; i++) CHECK(ilf_lock_try(&l, "0000", now, toy_sha) == ILF_LOCK_WRONG);
    CHECK(ilf_lock_try(&l, "0000", now, toy_sha) == ILF_LOCK_WAIT);
    CHECK(ilf_lock_try(&l, "2580", now + 1000, toy_sha) == ILF_LOCK_WAIT);
    CHECK(ilf_lock_wait_left(&l, now + 1000) == ILF_LOCK_WAIT_MS - 1000);
    CHECK(ilf_lock_try(&l, "2580", now + ILF_LOCK_WAIT_MS, toy_sha) == ILF_LOCK_OK);
    /* без PIN замка нет */
    ilf_lock_t open;
    ilf_lock_init(&open, NULL);
    CHECK(!ilf_lock_is_locked(&open, now));
}

int main(void)
{
    test_year_progress();
    test_date_ru();
    test_blinks();
    test_shake_scares_then_calms();
    test_many_shakes_make_sad();
    test_touch_looks_and_smiles();
    test_falls_asleep_and_wakes();
    test_hungry_on_low_battery();
    test_friend_message_happy();
    test_time_wraparound();
    test_lock();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all tests passed\n");
    return 0;
}
