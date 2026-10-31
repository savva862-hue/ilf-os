/*
 * ILF OS v0.4 — стадо, рация, iPhone, игры, Яндекс Диск.
 *
 * Потоки:
 *   ui-задача  — единственная, кто трогает LVGL: душа, лицо, часы, умный сон,
 *                а ещё стадо (ESP-NOW): знакомство, голосовые, рисунки.
 *   sensors    — гироскоп (встряска/наклон), кнопка, батарея → очередь событий.
 *   audio      — запись и проигрывание голосовых и рации (ilf_voice.c).
 *   cloud      — домашний Wi-Fi и выгрузка на Яндекс Диск (ilf_cloud.c).
 * Душа (ilf_soul) не знает про железо: получает события и отдаёт описание лица.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ilf_board.h"
#include "ilf_soul.h"
#include "ilf_ui.h"
#include "ilf_net.h"
#include "ilf_voice.h"
#include "ilf_ble.h"
#include "ilf_cloud.h"
#include "ilf_version.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "ilf";

/* Часовой пояс по умолчанию — Сеул. Подключится iPhone — пришлёт свой, и мы его запомним. */
#define ILF_DEFAULT_TZ "KST-9"
static char s_tz[32] = ILF_DEFAULT_TZ;

/* Экономия батареи: экран без дела приглушается, потом гаснет */
#define BRIGHT_LEVEL        0xC0
#define DIM_LEVEL           0x30
#define SCREEN_DIM_IDLE_MS  20000u
#define SCREEN_OFF_IDLE_MS  30000u

static QueueHandle_t s_events;
static ilf_soul_t s_soul;
/* наклон для игр (доли g), пишет задача датчиков */
static volatile float s_tilt_x, s_tilt_y;

/* ---------------- хозяин ---------------- */

#define FACTORY_RESET_HOLD_MS 10000u

typedef struct {
    char owner[ILF_HERD_NAME_LEN];
    char sheep[ILF_HERD_NAME_LEN];
} profile_t;

static profile_t s_profile;
static bool s_has_profile;
static volatile bool s_factory_reset;


static void profile_load(void)
{
    nvs_handle_t h;
    size_t size = sizeof s_profile;
    if (nvs_open("profile", NVS_READONLY, &h) == ESP_OK) {
        s_has_profile = nvs_get_blob(h, "me", &s_profile, &size) == ESP_OK && size == sizeof s_profile;
        nvs_close(h);
    }
}

static void profile_save(void)
{
    nvs_handle_t h;
    if (nvs_open("profile", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "me", &s_profile, sizeof s_profile);
    nvs_commit(h);
    nvs_close(h);
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "сброс к заводским: друзья, голосовые, хозяин");
    nvs_handle_t h;
    const char *spaces[] = { "profile", "herd", "cloud" };
    for (size_t i = 0; i < 3; i++) {
        if (nvs_open(spaces[i], NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    }
    esp_spiffs_format("storage");
    ilf_ble_forget();
    esp_restart();
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void post(ilf_event_t ev) { xQueueSend(s_events, &ev, 0); }

/* ---------------- датчики ---------------- */

static void sensors_task(void *arg)
{
    (void)arg;
    bool imu_ok = ilf_imu_init() == ESP_OK;
    float prev_mag = 1.f;
    uint32_t last_shake = 0, last_tilt = 0, last_batt = 0;
    bool btn_prev = false;
    uint32_t btn_down_at = 0;
    int16_t tilt_x_prev = 0, tilt_y_prev = 0;

    for (;;) {
        uint32_t t = now_ms();
        if (imu_ok) {
            ilf_accel_t a;
            if (ilf_imu_read(&a) == ESP_OK) {
                float mag = sqrtf(a.ax * a.ax + a.ay * a.ay + a.az * a.az);
                /* встряска: резкий скачок ускорения больше ~1.2g от покоя */
                if (fabsf(mag - 1.f) > 1.2f && fabsf(mag - prev_mag) > 0.8f && t - last_shake > 800) {
                    last_shake = t;
                    post((ilf_event_t){ .type = ILF_EV_SHAKE });
                }
                prev_mag = mag;
                /* наклон: куда «стекает» гравитация. Оси подбираем на живой плате. */
                s_tilt_x = a.ax;
                s_tilt_y = -a.ay;
                int16_t tx = (int16_t)(a.ax * 1000.f);
                int16_t ty = (int16_t)(-a.ay * 1000.f);
                if (t - last_tilt > 100 && (abs(tx - tilt_x_prev) > 60 || abs(ty - tilt_y_prev) > 60)) {
                    last_tilt = t;
                    tilt_x_prev = tx;
                    tilt_y_prev = ty;
                    post((ilf_event_t){ .type = ILF_EV_TILT, .tilt = { tx, ty } });
                }
            }
        }

        bool btn = ilf_button_pressed();
        if (btn && !btn_prev) {
            post((ilf_event_t){ .type = ILF_EV_BUTTON, .button = 0 });
            btn_down_at = t;
        }
        /* держать кнопку 10 секунд — сброс к заводским */
        if (btn && t - btn_down_at > FACTORY_RESET_HOLD_MS) s_factory_reset = true;
        btn_prev = btn;

        if (t - last_batt > 10000 || last_batt == 0) {
            last_batt = t;
            uint8_t pct;
            bool chg;
            if (ilf_power_battery(&pct, &chg) == ESP_OK) {
                post((ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { pct, chg } });
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20)); /* 50 Гц */
    }
}

/* ---------------- стадо ---------------- */

#define NOTE_MS       4000u
#define VOICE_DONE_MS 2500u

static ilf_herd_t *s_herd;
static int s_target = ILF_HERD_ALL;          /* кому: всё стадо или один друг */
static uint32_t s_note_until, s_voice_idle_at;
static int s_sent_ok;

static void pet_note(const char *text, uint32_t t)
{
    ilf_ui_pet_note(text);
    s_note_until = text ? t + NOTE_MS : 0;
}

static const char *name_by_mac(const uint8_t mac[6])
{
    int i = ilf_herd_find_friend(s_herd, mac);
    return i >= 0 ? ilf_herd_friend(s_herd, i)->name : NULL;
}

static void refresh_voice_list(void)
{
    ilf_ui_voice_item_t items[ILF_UI_VOICE_ROWS];
    int n = ilf_voice_items(items, ILF_UI_VOICE_ROWS);
    ilf_ui_voice_list(items, n);
}

static void refresh_target(void)
{
    if (s_target >= ilf_herd_friend_count(s_herd)) s_target = ILF_HERD_ALL;
    ilf_ui_set_target(s_target == ILF_HERD_ALL ? "всё стадо" : ilf_herd_friend(s_herd, s_target)->name);
    ilf_voice_state_t st = ilf_ui_live_on() ? ILF_VOICE_LIVE_READY : ILF_VOICE_IDLE;
    ilf_ui_voice_state(ilf_herd_friend_count(s_herd) ? st : ILF_VOICE_NO_FRIENDS, 0);
}

static void on_herd(const ilf_herd_event_t *ev, void *user)
{
    (void)user;
    uint32_t t = now_ms();
    const ilf_friend_t *f = ilf_herd_friend(s_herd, ev->friend_idx);
    char buf[64];
    switch (ev->type) {
    case ILF_HERD_EV_FRIEND_ADDED:
        snprintf(buf, sizeof buf, "Дружим с %s!", f ? f->name : "барашком");
        pet_note(buf, t);
        ilf_soul_handle(&s_soul, &(ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }, t);
        refresh_target();
        break;
    case ILF_HERD_EV_MESSAGE:
        if (ev->kind != ILF_MSG_VOICE || !f) break;
        ilf_voice_store(f->mac, f->name, ev->data, ev->len, false);
        refresh_voice_list();
        snprintf(buf, sizeof buf, "Голосовое от %s", f->name);
        pet_note(buf, t);
        ilf_soul_handle(&s_soul, &(ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }, t);
        post((ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }); /* разбудить экран */
        break;
    case ILF_HERD_EV_STROKE:
        ilf_ui_draw_remote(ev->friend_idx, ev->stroke);
        break;
    case ILF_HERD_EV_LIVE:
        /* рация: звук сразу в динамик; пустой пакет — друг просто в эфире */
        if (ev->len && f) {
            ilf_ui_live_from(f->name);
            ilf_voice_live_feed(ev->data, ev->len);
        }
        break;
    case ILF_HERD_EV_SENT:
        if (ev->ok) s_sent_ok++;
        if (!ilf_herd_sending(s_herd)) {
            ilf_ui_voice_state(s_sent_ok ? ILF_VOICE_SENT : ILF_VOICE_FAILED, 0);
            s_voice_idle_at = t + VOICE_DONE_MS;
        }
        break;
    default:
        break;
    }
}

/* колбэки экрана */
static void ui_record(bool start, void *user)
{
    (void)user;
    if (!ilf_herd_friend_count(s_herd)) { ilf_ui_voice_state(ILF_VOICE_NO_FRIENDS, 0); return; }
    if (start) {
        bool live = ilf_ui_live_on();
        if (!live && ilf_herd_sending(s_herd)) return;
        ilf_voice_record_start(live);
        ilf_ui_voice_state(live ? ILF_VOICE_LIVE_TALK : ILF_VOICE_RECORDING, 0);
    } else {
        ilf_voice_record_stop();
    }
}

static void ui_next_target(void *user)
{
    (void)user;
    int n = ilf_herd_friend_count(s_herd);
    s_target = s_target + 1 >= n ? ILF_HERD_ALL : s_target + 1;
    refresh_target();
}

static void ui_play(int item, void *user)
{
    (void)user;
    if (ilf_voice_play(item)) {
        ilf_ui_voice_state(ILF_VOICE_PLAYING, 0);
        refresh_voice_list();
    }
}

static void ui_stroke(const ilf_stroke_t *st, void *user)
{
    (void)user;
    ilf_herd_send_stroke(s_herd, s_target, st, now_ms());
}

static void ui_clear(void *user)
{
    (void)user;
    ilf_stroke_t st = { .clear = true };
    ilf_herd_send_stroke(s_herd, s_target, &st, now_ms());
}

static void ui_live(bool on, void *user)
{
    (void)user;
    if (!on && ilf_voice_recording()) ilf_voice_record_stop();
    refresh_target();
}

static void ui_save_drawing(const uint16_t *px, int w, int h, void *user)
{
    (void)user;
    ilf_voice_save_drawing(px, w, h);
}

/* ---------------- настройки ---------------- */

static int s_volume = 70;

static void ui_volume(int percent, void *user)
{
    (void)user;
    s_volume = percent;
    ilf_audio_set_volume((uint8_t)percent);
    nvs_handle_t h;
    if (nvs_open("profile", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "vol", (uint8_t)percent);
    nvs_commit(h);
    nvs_close(h);
}

static void volume_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("profile", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) s_volume = v;
        nvs_close(h);
    }
    ilf_audio_set_volume((uint8_t)s_volume);
}

static void ui_wifi_scan(void *user) { (void)user; ilf_cloud_scan(); }
static void ui_wifi_join(const char *ssid, const char *pass, void *user) { (void)user; ilf_cloud_join(ssid, pass); }
static void ui_wifi_forget(void *user) { (void)user; ilf_cloud_forget(); }
static void ui_disk(void *user) { (void)user; ilf_cloud_disk_tap(); }

static void cloud_frame(void)
{
    ilf_cloud_event_t ev;
    while (ilf_cloud_poll(&ev)) {
        switch (ev.type) {
        case ILF_CLOUD_EV_SCAN: {
            const char *list[ILF_CLOUD_SSIDS];
            for (int i = 0; i < ev.n; i++) list[i] = ev.ssids[i];
            ilf_ui_wifi_list(list, ev.n);
            break;
        }
        case ILF_CLOUD_EV_WIFI:      ilf_ui_wifi_status(ev.text); break;
        case ILF_CLOUD_EV_DISK:      ilf_ui_disk_status(ev.text); break;
        case ILF_CLOUD_EV_DISK_CODE: ilf_ui_disk_code(ev.url[0] ? ev.url : NULL, ev.code); break;
        case ILF_CLOUD_EV_SYNCED:    refresh_voice_list(); break;
        }
    }
}

/* ---------------- обновление от друга ---------------- */

static uint32_t s_reboot_at;

static void on_ota_answer(int answer, void *user)
{
    (void)user;
    ilf_net_ota_answer(answer);
    if (answer > 0) pet_note("Обновляюсь…", now_ms());
}

static void on_ota(const ilf_ota_event_t *ev, void *user)
{
    (void)user;
    uint32_t t = now_ms();
    char buf[96];
    switch (ev->type) {
    case ILF_OTA_EV_OFFER: {
        const char *who = name_by_mac(ev->mac);
        char title[32];
        snprintf(title, sizeof title, "ILF OS v%u", (unsigned)ev->version);
        snprintf(buf, sizeof buf, "%s делится новой версией. Обновить?", who ? who : "Друг");
        ilf_ui_ask("Обновление", title, buf, on_ota_answer, NULL);
        post((ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE });   /* разбудить экран, барашек рад */
        break;
    }
    case ILF_OTA_EV_PROGRESS:
        snprintf(buf, sizeof buf, "Обновляюсь… %d%%", (int)(ev->progress * 100));
        ilf_ui_pet_note(buf);
        s_note_until = t + NOTE_MS;
        break;
    case ILF_OTA_EV_DONE:
        pet_note("Обновился! Перезапускаюсь…", t);
        s_reboot_at = t + 2500;
        break;
    case ILF_OTA_EV_FAILED:
        pet_note("Обновление сорвалось, попробуем позже", t);
        break;
    }
}

/* раз в кадр: запись, отправка, подсказки */
static void herd_frame(uint32_t t)
{
    ilf_net_poll(t);

    static bool was_recording, was_playing, was_hearing;
    static uint16_t live_seq;
    static uint32_t live_ping;
    bool live = ilf_ui_live_on();
    if (ilf_voice_recording()) {
        ilf_ui_voice_state(live ? ILF_VOICE_LIVE_TALK : ILF_VOICE_RECORDING, ilf_voice_recorded_seconds());
    }
    /* рация: записанные блоки сразу в эфир */
    uint8_t blk[ILF_ADPCM_BLOCK_BYTES];
    while (ilf_voice_live_take(blk)) {
        ilf_herd_send_live(s_herd, s_target, live_seq++, blk, sizeof blk, t);
        live_ping = t;
    }
    /* рация включена — раз в 0.6 с «я в эфире», чтобы радио друзей не дремало */
    if (live && ilf_herd_friend_count(s_herd) && (int32_t)(t - live_ping) > 600) {
        live_ping = t;
        ilf_herd_send_live(s_herd, s_target, live_seq, NULL, 0, t);
    }
    bool hearing = ilf_voice_live_hearing();
    if (hearing && !ilf_voice_recording()) ilf_ui_voice_state(ILF_VOICE_LIVE_HEAR, 0);
    if (was_hearing && !hearing) refresh_target();
    was_hearing = hearing;
    uint8_t *rec;
    size_t len;
    if (ilf_voice_take_recording(&rec, &len)) {
        s_sent_ok = 0;
        static const uint8_t me[6] = { 0 };
        ilf_voice_store(me, "я", rec, len, true);   /* своё — только для Яндекс Диска */
        if (ilf_herd_send(s_herd, s_target, ILF_MSG_VOICE, rec, len, t)) {
            ilf_ui_voice_state(ILF_VOICE_SENDING, 0);
        } else {
            ilf_ui_voice_state(ILF_VOICE_FAILED, 0);
            s_voice_idle_at = t + VOICE_DONE_MS;
        }
        free(rec);
    } else if (was_recording && !ilf_voice_recording()) {
        refresh_target();   /* рация отпущена или слишком короткая запись — просто вернуться */
    }
    was_recording = ilf_voice_recording();
    if (ilf_herd_sending(s_herd)) ilf_ui_voice_state(ILF_VOICE_SENDING, ilf_herd_send_progress(s_herd));
    if (was_playing && !ilf_voice_playing()) refresh_target();
    was_playing = ilf_voice_playing();

    if (s_voice_idle_at && (int32_t)(t - s_voice_idle_at) > 0) {
        s_voice_idle_at = 0;
        refresh_target();
    }
    if (s_note_until && (int32_t)(t - s_note_until) > 0) pet_note(NULL, t);
}

/* первый запуск (в потоке экрана) */
static void on_setup(const ilf_setup_result_t *r, void *user)
{
    (void)user;
    memset(&s_profile, 0, sizeof s_profile);
    snprintf(s_profile.owner, sizeof s_profile.owner, "%s", r->owner);
    snprintf(s_profile.sheep, sizeof s_profile.sheep, "%s", r->sheep);
    profile_save();
    s_has_profile = true;
    ilf_herd_set_name(s_herd, s_profile.owner);
    ilf_ui_set_sheep_name(s_profile.sheep);
    ilf_ble_set_name(s_profile.sheep);
    ilf_ui_phone(ilf_ble_connected(), s_profile.sheep);
    ESP_LOGI(TAG, "хозяин %s, барашек %s", s_profile.owner, s_profile.sheep);
}

/* ---------------- iPhone по Bluetooth ---------------- */

static const char *ble_name(void) { return s_has_profile ? s_profile.sheep : "ILF"; }

static void ui_music(ilf_ui_music_btn_t btn, void *user)
{
    (void)user;
    static const ilf_ams_cmd_t map[] = { ILF_AMS_PREV, ILF_AMS_TOGGLE, ILF_AMS_NEXT,
                                         ILF_AMS_VOL_DOWN, ILF_AMS_VOL_UP };
    ilf_ble_music(map[btn]);
}

static void save_tz(const char *tz)
{
    if (!tz[0] || !strcmp(tz, s_tz)) return;
    snprintf(s_tz, sizeof s_tz, "%s", tz);
    nvs_handle_t h;
    if (nvs_open("profile", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "tz", s_tz);
    nvs_commit(h);
    nvs_close(h);
}

/* события с iPhone; true — разбудить экран */
static bool phone_frame(uint32_t t)
{
    bool wake = false;
    ilf_ble_event_t ev;
    while (ilf_ble_poll(&ev)) {
        switch (ev.type) {
        case ILF_BLE_EV_STATE:
            if (ev.state != ILF_BLE_PAIRING) ilf_ui_passkey(-1);
            ilf_ui_phone(ev.state == ILF_BLE_CONNECTED, ble_name());
            if (ev.state == ILF_BLE_CONNECTED) { pet_note("iPhone на связи", t); wake = true; }
            break;
        case ILF_BLE_EV_PASSKEY:
            ilf_ui_passkey((int32_t)ev.passkey);
            wake = true;
            break;
        case ILF_BLE_EV_TIME: {
            save_tz(ev.tz);
            /* в RTC — UTC, чтобы после перезагрузки без телефона часы шли верно */
            time_t now = time(NULL);
            struct tm utc;
            gmtime_r(&now, &utc);
            ilf_rtc_set(&utc);
            break;
        }
        case ILF_BLE_EV_MUSIC:
            ilf_ui_music(ev.music.title, ev.music.artist, ev.music.playing);
            break;
        case ILF_BLE_EV_NOTE: {
            if (ev.category == ILF_ANCS_CAT_CALL) break;   /* звонки на барашке не показываем */
            ilf_ui_notify(ilf_phone_app_name(ev.note.app), ev.note.title, ev.note.message, ev.note.uid);
            /* барашек замечает уведомление: смотрит наверх */
            ilf_soul_handle(&s_soul, &(ilf_event_t){ .type = ILF_EV_TOUCH, .touch = { ILF_SCREEN_W / 2, 40 } }, t);
            wake = true;
            break;
        }
        case ILF_BLE_EV_NOTE_GONE:
            ilf_ui_notify_gone(ev.uid);
            break;
        }
    }
    return wake;
}

/* ---------------- экран и душа ---------------- */

static void on_pet_touch(int16_t x, int16_t y, void *user)
{
    (void)user;
    ilf_soul_handle(&s_soul, &(ilf_event_t){ .type = ILF_EV_TOUCH, .touch = { x, y } }, now_ms());
}

static void ui_task(void *arg)
{
    (void)arg;
    lv_display_t *disp = ilf_display_init();
    ilf_touch_init(disp);

    ilf_soul_init(&s_soul, (uint32_t)esp_timer_get_time() ^ 0xA5A5u, now_ms());
    ilf_ui_init();
    ilf_ui_set_pet_touch_cb(on_pet_touch, NULL);

    s_herd = ilf_net_init(on_herd, NULL);
    ilf_net_set_ota_cb(on_ota, NULL);
    ilf_ui_herd_cb_t cb = { .record = ui_record, .next_target = ui_next_target, .play = ui_play,
                            .stroke = ui_stroke, .clear = ui_clear, .live = ui_live,
                            .save_drawing = ui_save_drawing };
    ilf_ui_set_herd_cb(&cb);
    refresh_target();
    refresh_voice_list();

    profile_load();
    volume_load();
    ilf_ui_settings_volume(s_volume);
    ilf_ui_set_version(ILF_FW_VERSION);
    ilf_ui_settings_cb_t scb = { .volume = ui_volume, .wifi_scan = ui_wifi_scan, .wifi_join = ui_wifi_join,
                                 .wifi_forget = ui_wifi_forget, .disk_link = ui_disk };
    ilf_ui_set_settings_cb(&scb);
    ilf_cloud_init(ble_name());
    ilf_ui_phone_cb_t pcb = { .music = ui_music };
    ilf_ui_set_phone_cb(&pcb);
    ilf_ui_phone(false, ble_name());
    ilf_ble_init(ble_name());
    if (s_has_profile) {
        ilf_herd_set_name(s_herd, s_profile.owner);
        ilf_ui_set_sheep_name(s_profile.sheep);
    } else {
        ilf_ui_setup_start(on_setup, NULL);
    }

    uint32_t last_clock = 0, last_activity = now_ms();
    bool screen_off = false, dimmed = false;

    bool marked_valid = false;
    for (;;) {
        uint32_t t = now_ms();

        /* новая прошивка прожила 30 с и не упала — оставляем её, иначе загрузчик откатит */
        if (!marked_valid && t > 30000) {
            marked_valid = true;
            esp_ota_mark_app_valid_cancel_rollback();
        }
        if (s_reboot_at && (int32_t)(t - s_reboot_at) > 0) esp_restart();

        ilf_event_t ev;
        bool woke = ilf_touch_take_wake();
        while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            ilf_soul_handle(&s_soul, &ev, t);
            /* удар о другого барашка — знакомство; без второго удара ничего не случится.
               В игре встряска — для шара предсказаний, не для знакомства. */
            if (ev.type == ILF_EV_SHAKE && ilf_ui_game_active()) ilf_ui_shake();
            else if (ev.type == ILF_EV_SHAKE && s_has_profile) ilf_herd_bump(s_herd, t);
            if (ev.type == ILF_EV_BATTERY) ilf_ui_update_battery(ev.battery.percent, ev.battery.charging);
            if (ev.type == ILF_EV_SHAKE || ev.type == ILF_EV_BUTTON || ev.type == ILF_EV_FRIEND_MESSAGE) woke = true;
        }
        /* пока пишем, слушаем или отправляем — экран не гаснет */
        if (ilf_voice_recording() || ilf_voice_playing() || ilf_herd_sending(s_herd) || ilf_voice_live_hearing() ||
            ilf_ui_game_active()) {
            woke = true;
        }
        if (woke) {
            last_activity = t;
            if (screen_off || dimmed) {
                if (screen_off && ilf_soul_is_asleep(&s_soul)) {
                    ilf_soul_handle(&s_soul, &(ilf_event_t){ .type = ILF_EV_BUTTON }, t);
                }
                if (screen_off) lv_obj_invalidate(lv_screen_active());
                screen_off = dimmed = false;
                ilf_display_set_brightness(BRIGHT_LEVEL);
            }
        }

        if (s_factory_reset) {
            ilf_display_set_brightness(BRIGHT_LEVEL);
            pet_note("Сбрасываю барашка…", t);
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(500));
            factory_reset();
        }
        herd_frame(t);
        cloud_frame();
        ilf_ui_tilt(s_tilt_x, s_tilt_y);
        /* выгрузка на Диск, когда барашек свободен: на зарядке чаще (скорее всего дома) */
        static uint32_t next_sync = 90000;
        if ((int32_t)(t - next_sync) > 0) {
            bool idle = !ilf_voice_recording() && !ilf_voice_playing() && !ilf_herd_sending(s_herd) &&
                        !ilf_ui_live_on() && !ilf_voice_live_hearing() && !ilf_herd_pairing(s_herd);
            if (idle && ilf_cloud_ready()) ilf_cloud_sync();
            next_sync = t + (s_soul.charging ? 20u : 120u) * 60u * 1000u;
        }
        if (phone_frame(t)) {
            last_activity = t;
            if (screen_off || dimmed) {
                if (screen_off) lv_obj_invalidate(lv_screen_active());
                screen_off = dimmed = false;
                ilf_display_set_brightness(BRIGHT_LEVEL);
            }
        }
        ilf_soul_update(&s_soul, t);

        /* экономия: без дела экран сперва приглушается, потом гаснет совсем */
        uint32_t idle = t - last_activity;
        if (!screen_off && idle > SCREEN_OFF_IDLE_MS) {
            screen_off = true;
            ilf_display_set_brightness(0);
        } else if (!screen_off && !dimmed && idle > SCREEN_DIM_IDLE_MS) {
            dimmed = true;
            ilf_display_set_brightness(DIM_LEVEL);
        }
        ilf_net_keep_listening(!screen_off || ilf_ui_live_on());

        if (screen_off) {
            /* экран тёмный: не рисуем, только душа, стадо и датчики */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ilf_ui_update_face(ilf_soul_face(&s_soul));
        if (t - last_clock >= 1000) {
            last_clock = t;
            time_t now = time(NULL);
            struct tm lt;
            localtime_r(&now, &lt);
            ilf_ui_update_clock(&lt);
        }

        uint32_t wait = lv_timer_handler();
        if (wait > 33) wait = 33;
        vTaskDelay(pdMS_TO_TICKS(wait ? wait : 1));
    }
}

static void time_init(void)
{
    /* часовой пояс, который последний раз прислал iPhone */
    nvs_handle_t h;
    size_t len = sizeof s_tz;
    if (nvs_open("profile", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "tz", s_tz, &len) != ESP_OK) snprintf(s_tz, sizeof s_tz, "%s", ILF_DEFAULT_TZ);
        nvs_close(h);
    }
    struct tm utc;
    if (ilf_rtc_init() == ESP_OK && ilf_rtc_get(&utc) == ESP_OK && utc.tm_year >= 125) {
        /* в RTC храним UTC */
        setenv("TZ", "UTC0", 1);
        tzset();
        time_t epoch = mktime(&utc);
        struct timeval tv = { .tv_sec = epoch };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "время взято из RTC");
    } else {
        ESP_LOGW(TAG, "в RTC нет времени, ждём iPhone");
    }
    setenv("TZ", s_tz, 1);
    tzset();
}

void app_main(void)
{
    ESP_LOGI(TAG, "ILF OS v%d — стадо, iPhone, обновления от друзей", ILF_FW_VERSION);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(ilf_board_init());
    ilf_power_init();
    ilf_button_init();
    time_init();
    if (ilf_audio_init() != ESP_OK) ESP_LOGW(TAG, "звук не поднялся, голосовые не работают");
    ilf_voice_init();
    ilf_power_save_init();

    s_events = xQueueCreate(32, sizeof(ilf_event_t));
    xTaskCreatePinnedToCore(ui_task, "ui", 12288, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(sensors_task, "sensors", 4096, NULL, 4, NULL, 0);
}
