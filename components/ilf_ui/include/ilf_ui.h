/*
 * ILF OS — экран. Зависит только от LVGL v9 и души, железа не знает,
 * поэтому тот же код рисует и на плате, и в симуляторе на ПК.
 *
 * Приложения листаются свайпом влево-вправо:
 *   Питомец · Голосовые · Рисунки · Музыка · Часы
 */
#pragma once

#include <stdint.h>
#include <time.h>

#include "lvgl.h"
#include "ilf_soul.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ILF_APP_PET = 0,
    ILF_APP_VOICE,
    ILF_APP_DRAW,
    ILF_APP_MUSIC,
    ILF_APP_CLOCK,
    ILF_APP_COUNT
} ilf_app_t;

typedef void (*ilf_ui_touch_cb_t)(int16_t x, int16_t y, void *user);

/* Создать все экраны на текущем дисплее LVGL. Вызывать из потока LVGL. */
void ilf_ui_init(void);

/* Касание барашка на экране «Питомец» (координаты экрана). */
void ilf_ui_set_pet_touch_cb(ilf_ui_touch_cb_t cb, void *user);

/* Перерисовать лицо по состоянию души. Дёшево, можно звать каждый кадр. */
void ilf_ui_update_face(const ilf_face_t *face);

/* Обновить часы, дату и «год прошёл на N%». Достаточно раз в секунду. */
void ilf_ui_update_clock(const struct tm *local_time);

/* Уровень заряда для строки состояния (0..100). */
void ilf_ui_update_battery(uint8_t percent, bool charging);

void ilf_ui_show_app(ilf_app_t app, bool animated);
ilf_app_t ilf_ui_current_app(void);

#ifdef __cplusplus
}
#endif
