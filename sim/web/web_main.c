/*
 * Тестер ILF OS в браузере: настоящий код души, экрана и первого запуска,
 * собранный в WebAssembly. Железо и второй барашек — понарошку (кнопки на странице).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "lvgl.h"
#include "ilf_clock.h"
#include "ilf_soul.h"
#include "ilf_ui.h"

#define EXPORT(name) __attribute__((export_name(#name)))
#define W ILF_SCREEN_W
#define H ILF_SCREEN_H

static uint32_t fb[W * H];
static uint32_t now_ms = 1;
static ilf_soul_t soul;
static lv_point_t ptr;
static bool ptr_down;

/* ---------- игрушечные «железо» и «друг» ---------- */

static const char *FRIEND = "Мира";
static bool has_friend;
static int target = -1;               /* -1 — всё стадо */
static bool recording;
static uint32_t rec_started, send_started, play_until, idle_at;
static bool sending;
static float rec_secs;
static ilf_ui_voice_item_t items[ILF_UI_VOICE_ROWS];
static int nitems;
static bool has_profile;
static char owner[ILF_HERD_NAME_LEN] = "", sheep[ILF_HERD_NAME_LEN] = "";
static uint32_t note_until;
static bool screen_off;

static void note(const char *t)
{
    ilf_ui_pet_note(t);
    note_until = t ? now_ms + 4000 : 0;
}

static void refresh_target(void)
{
    ilf_ui_set_target(target < 0 ? "всё стадо" : FRIEND);
    ilf_ui_voice_state(has_friend ? ILF_VOICE_IDLE : ILF_VOICE_NO_FRIENDS, 0);
}

static void add_item(const char *from, int secs)
{
    if (nitems < ILF_UI_VOICE_ROWS) nitems++;
    memmove(items + 1, items, sizeof items[0] * (ILF_UI_VOICE_ROWS - 1));
    snprintf(items[0].from, sizeof items[0].from, "%s", from);
    items[0].seconds = (uint16_t)secs;
    items[0].unplayed = true;
    ilf_ui_voice_list(items, nitems);
}

static void cb_record(bool start, void *u)
{
    (void)u;
    if (!has_friend) { ilf_ui_voice_state(ILF_VOICE_NO_FRIENDS, 0); return; }
    if (start && !sending) { recording = true; rec_started = now_ms; ilf_ui_voice_state(ILF_VOICE_RECORDING, 0); }
    else if (!start && recording) {
        recording = false;
        if (now_ms - rec_started < 300) { refresh_target(); return; }
        sending = true;
        send_started = now_ms;
    }
}
static void cb_next(void *u) { (void)u; target = (target < 0 && has_friend) ? 0 : -1; refresh_target(); }
static void cb_play(int i, void *u)
{
    (void)u;
    if (i >= nitems) return;
    items[i].unplayed = false;
    ilf_ui_voice_list(items, nitems);
    ilf_ui_voice_state(ILF_VOICE_PLAYING, 0);
    play_until = now_ms + 1000u * items[i].seconds;
}
static void cb_stroke(const ilf_stroke_t *s, void *u) { (void)s; (void)u; }
static void cb_clear(void *u) { (void)u; }

static void on_setup(const ilf_setup_result_t *r, void *u)
{
    (void)u;
    snprintf(owner, sizeof owner, "%s", r->owner);
    snprintf(sheep, sizeof sheep, "%s", r->sheep);
    has_profile = true;
    ilf_ui_set_sheep_name(sheep);
}

/* ---------- LVGL ---------- */

static uint32_t tick_cb(void) { return now_ms; }
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) { (void)a; (void)px; lv_display_flush_ready(d); }
static void read_cb(lv_indev_t *i, lv_indev_data_t *d)
{
    (void)i;
    d->point = ptr;
    d->state = ptr_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void on_touch(int16_t x, int16_t y, void *u)
{
    (void)u;
    ilf_event_t ev = { .type = ILF_EV_TOUCH, .touch = { x, y } };
    ilf_soul_handle(&soul, &ev, now_ms);
}

EXPORT(ilf_web_fb) uint32_t *ilf_web_fb(void) { return fb; }

EXPORT(ilf_web_init) void ilf_web_init(uint32_t seed)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(disp, fb, NULL, sizeof fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);

    ilf_soul_init(&soul, seed, now_ms);
    ilf_ui_init();
    ilf_ui_set_pet_touch_cb(on_touch, NULL);
    ilf_ui_herd_cb_t cb = { .record = cb_record, .next_target = cb_next, .play = cb_play,
                            .stroke = cb_stroke, .clear = cb_clear };
    ilf_ui_set_herd_cb(&cb);
    ilf_ui_update_battery(83, false);
    refresh_target();
    ilf_ui_setup_start(on_setup, NULL);
}

EXPORT(ilf_web_clock) void ilf_web_clock(int year, int mon, int mday, int hour, int min, int wday, int yday)
{
    struct tm t = { 0 };
    t.tm_year = year - 1900; t.tm_mon = mon; t.tm_mday = mday; t.tm_hour = hour; t.tm_min = min;
    t.tm_wday = wday; t.tm_yday = yday;
    ilf_ui_update_clock(&t);
}

EXPORT(ilf_web_pointer) void ilf_web_pointer(int x, int y, int down)
{
    ptr.x = x; ptr.y = y; ptr_down = down != 0;
}

/* продвинуть время; вернуть 1, если кадр надо перерисовать */
EXPORT(ilf_web_frame) int ilf_web_frame(uint32_t dt)
{
    now_ms += dt;
    if (screen_off) return 0;
    ilf_soul_update(&soul, now_ms);
    ilf_ui_update_face(ilf_soul_face(&soul));

    if (recording) {
        rec_secs = (float)(now_ms - rec_started) / 1000.f;
        ilf_ui_voice_state(ILF_VOICE_RECORDING, rec_secs);
        if (rec_secs > 17) cb_record(false, NULL);
    }
    if (sending) {
        float p = (float)(now_ms - send_started) / 1500.f;
        if (p >= 1) { sending = false; ilf_ui_voice_state(ILF_VOICE_SENT, 0); idle_at = now_ms + 2500; }
        else ilf_ui_voice_state(ILF_VOICE_SENDING, p);
    }
    if (play_until && (int32_t)(now_ms - play_until) > 0) { play_until = 0; refresh_target(); }
    if (idle_at && (int32_t)(now_ms - idle_at) > 0) { idle_at = 0; refresh_target(); }
    if (note_until && (int32_t)(now_ms - note_until) > 0) note(NULL);

    lv_timer_handler();
    return 1;
}

/* кнопки на странице */
enum { WEB_SHAKE = 1, WEB_BUMP, WEB_FRIEND_VOICE, WEB_FRIEND_DRAW, WEB_BATTERY_LOW, WEB_BATTERY_OK,
       WEB_SCREEN_OFF, WEB_SCREEN_ON, WEB_RESET };

EXPORT(ilf_web_event) void ilf_web_event(int what)
{
    switch (what) {
    case WEB_SHAKE:
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_SHAKE }, now_ms);
        break;
    case WEB_BUMP:
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_SHAKE }, now_ms);
        if (!has_profile) break;
        has_friend = true;
        note("Дружим с Мира!");
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }, now_ms + 1);
        refresh_target();
        break;
    case WEB_FRIEND_VOICE:
        if (!has_friend) { note("Сначала подружись: «Стукнуться»"); break; }
        add_item(FRIEND, 3 + (int)(now_ms % 9));
        note("Голосовое от Мира");
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }, now_ms);
        break;
    case WEB_FRIEND_DRAW: {
        if (!has_friend) { note("Сначала подружись: «Стукнуться»"); break; }
        /* друг рисует сердечко */
        ilf_stroke_t s = { .stroke_id = (uint8_t)now_ms };
        for (int i = 0; i <= 36 && s.count < ILF_HERD_STROKE_POINTS; i++) {
            float t = (float)i / 36.f * 6.2831853f;
            float sx = sinf(t), x = 16 * sx * sx * sx;
            float y = 13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t);
            s.pts[s.count].x = (uint16_t)(160 + x * 5.5f);
            s.pts[s.count].y = (uint16_t)(112 - y * 5.5f);
            s.count++;
        }
        ilf_ui_draw_remote(0, &s);
        ilf_ui_show_app(ILF_APP_DRAW, true);
        break;
    }
    case WEB_BATTERY_LOW:
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { 12, false } }, now_ms);
        ilf_ui_update_battery(12, false);
        break;
    case WEB_BATTERY_OK:
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { 83, false } }, now_ms);
        ilf_ui_update_battery(83, false);
        break;
    case WEB_SCREEN_OFF:
        screen_off = true;
        memset(fb, 0, sizeof fb);
        break;
    case WEB_SCREEN_ON:
        if (!screen_off) break;
        screen_off = false;
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_BUTTON }, now_ms);
        lv_obj_invalidate(lv_screen_active());
        break;
    }
}
