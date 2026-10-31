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
#include "ilf_herd.h"

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

/* ---------- стадо: голосовые, рисунки, знакомство ---------- */

typedef enum {
    ILF_VOICE_IDLE = 0,
    ILF_VOICE_NO_FRIENDS,   /* друзей нет: подсказка «стукнись с другом» */
    ILF_VOICE_RECORDING,    /* value = секунды записи */
    ILF_VOICE_SENDING,      /* value = ход отправки 0..1 */
    ILF_VOICE_SENT,
    ILF_VOICE_FAILED,
    ILF_VOICE_PLAYING,
} ilf_voice_state_t;

typedef struct {
    char     from[ILF_HERD_NAME_LEN];
    uint16_t seconds;
    bool     unplayed;
} ilf_ui_voice_item_t;

#define ILF_UI_VOICE_ROWS 3

typedef struct {
    void (*record)(bool start, void *user);          /* зажали / отпустили кнопку записи */
    void (*next_target)(void *user);                 /* тап по «Кому» */
    void (*play)(int item, void *user);              /* тап по голосовому из списка */
    void (*stroke)(const ilf_stroke_t *s, void *user); /* кусок своего рисунка */
    void (*clear)(void *user);                       /* очистили холст */
    void *user;
} ilf_ui_herd_cb_t;

void ilf_ui_set_herd_cb(const ilf_ui_herd_cb_t *cb);
/* Кому уходят голосовые и рисунки: «Всё стадо» или имя друга. */
void ilf_ui_set_target(const char *name);
void ilf_ui_voice_state(ilf_voice_state_t st, float value);
/* Последние голосовые, новые первыми (до ILF_UI_VOICE_ROWS). */
void ilf_ui_voice_list(const ilf_ui_voice_item_t *items, int count);
/* Рисунок друга и очистка холста. */
void ilf_ui_draw_remote(int friend_idx, const ilf_stroke_t *s);
void ilf_ui_draw_clear(void);
/* ---------- первый запуск ---------- */

typedef struct {
    char owner[ILF_HERD_NAME_LEN];   /* как зовут хозяина — так барашка видят друзья */
    char sheep[ILF_HERD_NAME_LEN];   /* как зовут самого барашка */
} ilf_setup_result_t;

typedef void (*ilf_ui_setup_cb)(const ilf_setup_result_t *r, void *user);

/* Знакомство с хозяином: привет → имя хозяина → имя барашка → готово. */
void ilf_ui_setup_start(ilf_ui_setup_cb done, void *user);
bool ilf_ui_setup_active(void);
/* Имя барашка в углу экрана питомца */
void ilf_ui_set_sheep_name(const char *name);

/* Строка про стадо на экране питомца («Знакомимся…»). NULL — убрать. */
void ilf_ui_pet_note(const char *text);
ilf_app_t ilf_ui_current_app(void);

#ifdef __cplusplus
}
#endif
