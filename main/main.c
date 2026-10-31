/*
 * ILF OS v0.2 — «Стадо».
 *
 * Потоки:
 *   ui-задача  — единственная, кто трогает LVGL: душа, лицо, часы, умный сон,
 *                а ещё стадо (ESP-NOW): знакомство, голосовые, рисунки.
 *   sensors    — гироскоп (встряска/наклон), кнопка, батарея → очередь событий.
 *   audio      — запись и проигрывание голосовых (ilf_voice.c).
 * Душа (ilf_soul) не знает про железо: получает события и отдаёт описание лица.
 */
#include <stdio.h>
#include <stdlib.h>
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

static const char *TAG = "ilf";

/* Часовой пояс по умолчанию — Сеул. Позже его будет присылать iPhone по Bluetooth. */
#define ILF_DEFAULT_TZ "KST-9"

/* Экономия батареи: экран без дела приглушается, потом гаснет */
#define BRIGHT_LEVEL        0xC0
#define DIM_LEVEL           0x30
#define SCREEN_DIM_IDLE_MS  20000u
#define SCREEN_OFF_IDLE_MS  30000u

static QueueHandle_t s_events;
static ilf_soul_t s_soul;

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
        if (btn && !btn_prev) post((ilf_event_t){ .type = ILF_EV_BUTTON, .button = 0 });
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
    int n = ilf_voice_items(items, ILF_UI_VOICE_ROWS, name_by_mac);
    ilf_ui_voice_list(items, n);
}

static void refresh_target(void)
{
    if (s_target >= ilf_herd_friend_count(s_herd)) s_target = ILF_HERD_ALL;
    ilf_ui_set_target(s_target == ILF_HERD_ALL ? "всё стадо" : ilf_herd_friend(s_herd, s_target)->name);
    ilf_ui_voice_state(ilf_herd_friend_count(s_herd) ? ILF_VOICE_IDLE : ILF_VOICE_NO_FRIENDS, 0);
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
        ilf_voice_store(f->mac, ev->data, ev->len);
        refresh_voice_list();
        snprintf(buf, sizeof buf, "Голосовое от %s", f->name);
        pet_note(buf, t);
        ilf_soul_handle(&s_soul, &(ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }, t);
        post((ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE }); /* разбудить экран */
        break;
    case ILF_HERD_EV_STROKE:
        ilf_ui_draw_remote(ev->friend_idx, ev->stroke);
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
        if (ilf_herd_sending(s_herd)) return;
        ilf_voice_record_start();
        ilf_ui_voice_state(ILF_VOICE_RECORDING, 0);
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

/* раз в кадр: запись, отправка, подсказки */
static void herd_frame(uint32_t t)
{
    ilf_net_poll(t);

    static bool was_recording, was_playing;
    if (ilf_voice_recording()) {
        ilf_ui_voice_state(ILF_VOICE_RECORDING, ilf_voice_recorded_seconds());
    }
    uint8_t *rec;
    size_t len;
    if (ilf_voice_take_recording(&rec, &len)) {
        s_sent_ok = 0;
        if (ilf_herd_send(s_herd, s_target, ILF_MSG_VOICE, rec, len, t)) {
            ilf_ui_voice_state(ILF_VOICE_SENDING, 0);
        } else {
            ilf_ui_voice_state(ILF_VOICE_FAILED, 0);
            s_voice_idle_at = t + VOICE_DONE_MS;
        }
        free(rec);
    } else if (was_recording && !ilf_voice_recording()) {
        refresh_target();   /* слишком короткая запись — просто вернуться */
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
    ilf_ui_herd_cb_t cb = { .record = ui_record, .next_target = ui_next_target, .play = ui_play,
                            .stroke = ui_stroke, .clear = ui_clear };
    ilf_ui_set_herd_cb(&cb);
    refresh_target();
    refresh_voice_list();

    uint32_t last_clock = 0, last_activity = now_ms();
    bool screen_off = false, dimmed = false;

    for (;;) {
        uint32_t t = now_ms();

        ilf_event_t ev;
        bool woke = ilf_touch_take_wake();
        while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            ilf_soul_handle(&s_soul, &ev, t);
            /* удар о другого барашка — знакомство; без второго удара ничего не случится */
            if (ev.type == ILF_EV_SHAKE) ilf_herd_bump(s_herd, t);
            if (ev.type == ILF_EV_BATTERY) ilf_ui_update_battery(ev.battery.percent, ev.battery.charging);
            if (ev.type == ILF_EV_SHAKE || ev.type == ILF_EV_BUTTON || ev.type == ILF_EV_FRIEND_MESSAGE) woke = true;
        }
        /* пока пишем, слушаем или отправляем — экран не гаснет */
        if (ilf_voice_recording() || ilf_voice_playing() || ilf_herd_sending(s_herd)) woke = true;
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

        herd_frame(t);
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
        ilf_net_keep_listening(!screen_off);

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
    setenv("TZ", ILF_DEFAULT_TZ, 1);
    tzset();
    struct tm utc;
    if (ilf_rtc_init() == ESP_OK && ilf_rtc_get(&utc) == ESP_OK && utc.tm_year >= 125) {
        /* в RTC храним UTC */
        setenv("TZ", "UTC0", 1);
        tzset();
        time_t epoch = mktime(&utc);
        setenv("TZ", ILF_DEFAULT_TZ, 1);
        tzset();
        struct timeval tv = { .tv_sec = epoch };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "время взято из RTC");
    } else {
        ESP_LOGW(TAG, "в RTC нет времени, ждём iPhone");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ILF OS v0.2 — стадо");
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
