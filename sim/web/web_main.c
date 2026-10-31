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
static bool phone_on;

static void note(const char *t)
{
    ilf_ui_pet_note(t);
    note_until = t ? now_ms + 4000 : 0;
}

static uint32_t hear_until;          /* друг говорит в рацию */
static bool charging;
static uint8_t battery = 83;

static void refresh_target(void)
{
    ilf_ui_set_target(target < 0 ? "всё стадо" : FRIEND);
    ilf_voice_state_t st = ilf_ui_live_on() ? ILF_VOICE_LIVE_READY : ILF_VOICE_IDLE;
    ilf_ui_voice_state(has_friend ? st : ILF_VOICE_NO_FRIENDS, 0);
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
    bool live = ilf_ui_live_on();
    if (start && !sending) {
        recording = true;
        rec_started = now_ms;
        ilf_ui_voice_state(live ? ILF_VOICE_LIVE_TALK : ILF_VOICE_RECORDING, 0);
    } else if (!start && recording) {
        recording = false;
        if (live || now_ms - rec_started < 300) { refresh_target(); return; }
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
static void cb_live(bool on, void *u) { (void)u; (void)on; recording = false; refresh_target(); }

/* ---------- игрушечные Wi-Fi и Яндекс Диск ---------- */

static char wifi_ssid[33];
static bool disk_linked;
static uint32_t scan_at, join_at, disk_code_until, disk_sync_until;

static void cb_volume(int p, void *u)
{
    (void)u;
    char buf[48];
    snprintf(buf, sizeof buf, "Громкость динамика %d%%", p);
    note(buf);
}
static void cb_wifi_scan(void *u) { (void)u; scan_at = now_ms + 1200; }
static void cb_wifi_join(const char *ssid, const char *pass, void *u)
{
    (void)u; (void)pass;
    snprintf(wifi_ssid, sizeof wifi_ssid, "%s", ssid);
    join_at = now_ms + 1500;
}
static void cb_wifi_forget(void *u) { (void)u; wifi_ssid[0] = 0; disk_linked = false; ilf_ui_wifi_status(NULL); ilf_ui_disk_status(NULL); }
static void cb_disk(void *u)
{
    (void)u;
    if (!wifi_ssid[0] || join_at) { ilf_ui_disk_status("сначала Wi-Fi"); return; }
    if (!disk_linked) {
        ilf_ui_disk_code("ya.ru/device", "4821 3307");
        disk_code_until = now_ms + 5000;
    } else {
        ilf_ui_disk_status("выгружаю 1 из 2…");
        disk_sync_until = now_ms + 2500;
    }
}

static void on_setup(const ilf_setup_result_t *r, void *u)
{
    (void)u;
    snprintf(owner, sizeof owner, "%s", r->owner);
    snprintf(sheep, sizeof sheep, "%s", r->sheep);
    ilf_ui_phone(phone_on, sheep);
    has_profile = true;
    ilf_ui_set_sheep_name(sheep);
}

/* ---------- игрушечный iPhone ---------- */

static const char *TRACKS[][2] = {
    { "Группа крови", "Кино" }, { "Dynamite", "BTS" }, { "Blinding Lights", "The Weeknd" },
};
static int track;
static bool music_playing;
static uint32_t pair_until;

static void show_track(void) { ilf_ui_music(TRACKS[track][0], TRACKS[track][1], music_playing); }

static void cb_music(ilf_ui_music_btn_t btn, void *u)
{
    (void)u;
    if (btn == ILF_UI_MUSIC_TOGGLE) music_playing = !music_playing;
    if (btn == ILF_UI_MUSIC_NEXT) track = (track + 1) % 3;
    if (btn == ILF_UI_MUSIC_PREV) track = (track + 2) % 3;
    show_track();
}

/* ---------- игрушечное обновление от Миры ---------- */

static uint32_t upd_started;

static void on_update(int answer, void *u)
{
    (void)u;
    if (answer > 0) upd_started = now_ms;
    else if (answer == 0) note("Ладно, останемся на этой версии");
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
                            .stroke = cb_stroke, .clear = cb_clear, .live = cb_live };
    ilf_ui_set_herd_cb(&cb);
    ilf_ui_phone_cb_t pcb = { .music = cb_music };
    ilf_ui_set_phone_cb(&pcb);
    ilf_ui_phone(false, "ILF");
    ilf_ui_update_battery(battery, charging);
    ilf_ui_settings_cb_t scb = { .volume = cb_volume, .wifi_scan = cb_wifi_scan, .wifi_join = cb_wifi_join,
                                 .wifi_forget = cb_wifi_forget, .disk_link = cb_disk };
    ilf_ui_set_settings_cb(&scb);
    ilf_ui_set_version(4);
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

/* наклон с клавиатуры (стрелки), тысячные доли g */
EXPORT(ilf_web_tilt) void ilf_web_tilt(int x, int y)
{
    ilf_ui_tilt((float)x / 1000.f, (float)y / 1000.f);
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
        ilf_ui_voice_state(ilf_ui_live_on() ? ILF_VOICE_LIVE_TALK : ILF_VOICE_RECORDING, rec_secs);
        if (!ilf_ui_live_on() && rec_secs > 60) cb_record(false, NULL);
    }
    if (hear_until) {
        if ((int32_t)(now_ms - hear_until) > 0) { hear_until = 0; refresh_target(); }
        else if (!recording) ilf_ui_voice_state(ILF_VOICE_LIVE_HEAR, 0);
    }
    if (scan_at && (int32_t)(now_ms - scan_at) > 0) {
        scan_at = 0;
        static const char *nets[] = { "Home", "Savva_5G", "KT_GiGA_2G_Wave", "iptime", "U+Net3F21" };
        ilf_ui_wifi_list(nets, 5);
    }
    if (join_at && (int32_t)(now_ms - join_at) > 0) {
        join_at = 0;
        char buf[48];
        snprintf(buf, sizeof buf, "%s · ок", wifi_ssid);
        ilf_ui_wifi_status(buf);
    }
    if (disk_code_until && (int32_t)(now_ms - disk_code_until) > 0) {
        disk_code_until = 0;
        disk_linked = true;
        ilf_ui_disk_code(NULL, NULL);
        ilf_ui_disk_status("привязан · всё выгружено");
        note("Яндекс Диск привязан");
    }
    if (disk_sync_until && (int32_t)(now_ms - disk_sync_until) > 0) {
        disk_sync_until = 0;
        ilf_ui_disk_status("привязан · всё выгружено");
    }
    if (sending) {
        float p = (float)(now_ms - send_started) / 1500.f;
        if (p >= 1) { sending = false; ilf_ui_voice_state(ILF_VOICE_SENT, 0); idle_at = now_ms + 2500; }
        else ilf_ui_voice_state(ILF_VOICE_SENDING, p);
    }
    if (play_until && (int32_t)(now_ms - play_until) > 0) { play_until = 0; refresh_target(); }
    if (idle_at && (int32_t)(now_ms - idle_at) > 0) { idle_at = 0; refresh_target(); }
    if (note_until && (int32_t)(now_ms - note_until) > 0) note(NULL);
    if (upd_started) {
        uint32_t el = now_ms - upd_started;
        char buf[48];
        if (el < 6000) { snprintf(buf, sizeof buf, "Обновляюсь… %d%%", (int)(el / 60)); ilf_ui_pet_note(buf); note_until = now_ms + 1000; }
        else { upd_started = 0; note("Обновился! (в тестере понарошку)"); }
    }
    if (pair_until && (int32_t)(now_ms - pair_until) > 0) {
        /* «ввёл код на iPhone» */
        pair_until = 0;
        phone_on = true;
        ilf_ui_passkey(-1);
        ilf_ui_phone(true, sheep[0] ? sheep : "ILF");
        music_playing = true;
        show_track();
        note("iPhone на связи");
    }

    lv_timer_handler();
    return 1;
}

/* кнопки на странице */
enum { WEB_SHAKE = 1, WEB_BUMP, WEB_FRIEND_VOICE, WEB_FRIEND_DRAW, WEB_BATTERY_LOW, WEB_BATTERY_OK,
       WEB_SCREEN_OFF, WEB_SCREEN_ON, WEB_RESET, WEB_PHONE, WEB_PHONE_NOTE, WEB_UNUSED_12, WEB_UNUSED_13, WEB_FRIEND_UPDATE,
       WEB_CHARGE, WEB_FRIEND_LIVE };

EXPORT(ilf_web_event) void ilf_web_event(int what)
{
    switch (what) {
    case WEB_SHAKE:
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_SHAKE }, now_ms);
        ilf_ui_shake();
        break;
    case WEB_CHARGE:
        charging = !charging;
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { battery, charging } }, now_ms);
        ilf_ui_update_battery(battery, charging);
        break;
    case WEB_FRIEND_LIVE:
        if (!has_friend) { note("Сначала подружись: «Стукнуться»"); break; }
        if (recording) break;
        ilf_ui_live_from(FRIEND);
        hear_until = now_ms + 3500;
        ilf_ui_show_app(ILF_APP_VOICE, true);
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
        battery = 12;
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { battery, charging } }, now_ms);
        ilf_ui_update_battery(battery, charging);
        break;
    case WEB_BATTERY_OK:
        battery = 83;
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { battery, charging } }, now_ms);
        ilf_ui_update_battery(battery, charging);
        break;
    case WEB_PHONE:
        if (phone_on) {
            phone_on = false;
            ilf_ui_phone(false, sheep[0] ? sheep : "ILF");
            note("iPhone отключился");
        } else if (!pair_until) {
            ilf_ui_passkey((int32_t)(100000 + now_ms % 900000));
            pair_until = now_ms + 3500;
        }
        break;
    case WEB_PHONE_NOTE:
        if (!phone_on) { note("Сначала подключи iPhone"); break; }
        ilf_ui_notify("Telegram", "Мама", "Ты поел? Не забудь шапку, на улице холодно", 1);
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_TOUCH, .touch = { W / 2, 40 } }, now_ms);
        break;
    case WEB_FRIEND_UPDATE:
        if (!has_friend) { note("Сначала подружись: «Стукнуться»"); break; }
        ilf_ui_ask("Обновление", "ILF OS v5", "Мира делится новой версией. Обновить?", on_update, NULL);
        ilf_soul_handle(&soul, &(ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }, now_ms);
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
