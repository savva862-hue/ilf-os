/*
 * ILF OS v0.1 — «Живой барашек».
 *
 * Потоки:
 *   ui-задача  — единственная, кто трогает LVGL: душа, лицо, часы, умный сон.
 *   sensors    — гироскоп (встряска/наклон), кнопка, батарея → очередь событий.
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

static const char *TAG = "ilf";

/* Часовой пояс по умолчанию — Сеул. Позже его будет присылать iPhone по Bluetooth. */
#define ILF_DEFAULT_TZ "KST-9"

/* Умный сон: через сколько после засыпания барашка гасить экран */
#define DIM_LEVEL          0x30
#define SCREEN_OFF_AFTER_MS 30000u

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

    uint32_t last_clock = 0, asleep_since = 0;
    bool screen_off = false;

    for (;;) {
        uint32_t t = now_ms();

        ilf_event_t ev;
        bool woke = ilf_touch_take_wake();
        while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            ilf_soul_handle(&s_soul, &ev, t);
            if (ev.type == ILF_EV_BATTERY) ilf_ui_update_battery(ev.battery.percent, ev.battery.charging);
            if (ev.type == ILF_EV_SHAKE || ev.type == ILF_EV_BUTTON || ev.type == ILF_EV_FRIEND_MESSAGE) woke = true;
        }
        if (woke && screen_off) {
            ilf_soul_handle(&s_soul, &(ilf_event_t){ .type = ILF_EV_BUTTON }, t);
        }

        ilf_soul_update(&s_soul, t);
        ilf_ui_update_face(ilf_soul_face(&s_soul));

        if (t - last_clock >= 1000) {
            last_clock = t;
            time_t now = time(NULL);
            struct tm lt;
            localtime_r(&now, &lt);
            ilf_ui_update_clock(&lt);
        }

        /* умный сон: барашек уснул → приглушить, спит долго → погасить экран */
        if (ilf_soul_is_asleep(&s_soul)) {
            if (!asleep_since) {
                asleep_since = t;
                ilf_display_set_brightness(DIM_LEVEL);
            } else if (!screen_off && t - asleep_since > SCREEN_OFF_AFTER_MS) {
                screen_off = true;
                ilf_display_set_brightness(0);
            }
        } else if (asleep_since) {
            asleep_since = 0;
            screen_off = false;
            ilf_display_set_brightness(0xC0);
        }

        uint32_t wait = lv_timer_handler();
        if (wait > 33) wait = 33;
        if (screen_off) wait = 100; /* экран тёмный — просыпаемся реже */
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
    ESP_LOGI(TAG, "ILF OS v0.1 — живой барашек");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(ilf_board_init());
    ilf_power_init();
    ilf_button_init();
    time_init();

    s_events = xQueueCreate(32, sizeof(ilf_event_t));
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(sensors_task, "sensors", 4096, NULL, 4, NULL, 0);
}
