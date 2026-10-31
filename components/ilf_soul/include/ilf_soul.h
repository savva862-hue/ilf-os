/*
 * ILF OS — «душа» барашка.
 *
 * Чистый C99 без зависимостей от железа, FreeRTOS и LVGL.
 * Душа получает события (касание, встряска, наклон, заряд, сообщение от друга)
 * и время в миллисекундах, а наружу отдаёт только описание лица (ilf_face_t).
 * Поэтому её можно гонять в тестах и симуляторе на ПК.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ILF_EMO_CALM = 0,   /* обычный, моргает, смотрит по сторонам */
    ILF_EMO_HAPPY,      /* радуется: сообщение от друга, внимание */
    ILF_EMO_SAD,        /* грустит: его трясли слишком часто */
    ILF_EMO_SCARED,     /* испуган: встряска или удар */
    ILF_EMO_SLEEPY,     /* засыпает: давно никто не трогал */
    ILF_EMO_ASLEEP,     /* спит: глаза закрыты, экран можно гасить */
    ILF_EMO_HUNGRY,     /* голоден: садится батарея */
    ILF_EMO_COUNT
} ilf_emotion_t;

typedef enum {
    ILF_EV_TOUCH = 0,       /* касание экрана, x/y в координатах экрана */
    ILF_EV_SHAKE,           /* встряска (гироскоп) */
    ILF_EV_TILT,            /* наклон, tilt_x/tilt_y от -1000 до 1000 */
    ILF_EV_BUTTON,          /* нажатие физической кнопки */
    ILF_EV_BATTERY,         /* уровень заряда, 0..100, и признак зарядки */
    ILF_EV_FRIEND_MESSAGE,  /* пришло голосовое или рисунок от друга */
} ilf_event_type_t;

typedef struct {
    ilf_event_type_t type;
    union {
        struct { int16_t x, y; } touch;
        struct { int16_t x, y; } tilt;
        struct { uint8_t percent; bool charging; } battery;
        uint8_t button;
    };
} ilf_event_t;

/* Всё, что нужно экрану, чтобы нарисовать лицо. */
typedef struct {
    ilf_emotion_t emotion;
    float gaze_x;     /* куда смотрят зрачки: -1 (влево) .. 1 (вправо) */
    float gaze_y;     /* -1 (вверх) .. 1 (вниз) */
    float eyelid;     /* 0 = глаза открыты, 1 = закрыты */
    float mouth;      /* -1 = грустный рот, 0 = ровный, 1 = улыбка */
    bool  mouth_open; /* «о» от испуга */
} ilf_face_t;

/* Тайминги в миллисекундах. Вынесены наружу, чтобы их было легко подкручивать. */
#define ILF_SOUL_BLINK_MIN_MS        2000u
#define ILF_SOUL_BLINK_MAX_MS        6000u
#define ILF_SOUL_BLINK_LEN_MS         160u
#define ILF_SOUL_HAPPY_MS            3000u
#define ILF_SOUL_FRIEND_HAPPY_MS     4500u
#define ILF_SOUL_SCARED_MS           2500u
#define ILF_SOUL_SAD_MS              6000u
#define ILF_SOUL_SLEEPY_AFTER_MS    60000u
#define ILF_SOUL_ASLEEP_AFTER_MS   120000u
#define ILF_SOUL_LOOK_AT_MS          1500u
#define ILF_SOUL_SHAKES_TO_SAD          3u
#define ILF_SOUL_SHAKE_WINDOW_MS    10000u
#define ILF_SOUL_HUNGRY_PERCENT        15u

typedef struct {
    /* состояние, руками не трогать */
    uint32_t rng;
    uint32_t now_ms;
    uint32_t last_activity_ms;
    ilf_emotion_t transient;        /* временная эмоция поверх базовой */
    uint32_t transient_until_ms;
    uint32_t next_blink_ms;
    uint32_t blink_started_ms;
    bool     blinking;
    uint32_t shake_times[ILF_SOUL_SHAKES_TO_SAD];
    uint8_t  shake_idx;
    uint8_t  shake_count;
    uint8_t  battery_percent;
    bool     charging;
    float    target_gaze_x, target_gaze_y;
    uint32_t look_until_ms;         /* смотрит в точку касания до этого момента */
    uint32_t next_wander_ms;
    ilf_face_t face;
} ilf_soul_t;

void ilf_soul_init(ilf_soul_t *s, uint32_t seed, uint32_t now_ms);

/* Подать событие. now_ms — монотонное время (можно с переполнением). */
void ilf_soul_handle(ilf_soul_t *s, const ilf_event_t *ev, uint32_t now_ms);

/* Продвинуть время: моргание, затухание эмоций, засыпание. Вызывать ~30 раз в секунду. */
void ilf_soul_update(ilf_soul_t *s, uint32_t now_ms);

const ilf_face_t *ilf_soul_face(const ilf_soul_t *s);

/* Барашек спит — экран можно приглушить или погасить. */
bool ilf_soul_is_asleep(const ilf_soul_t *s);

const char *ilf_emotion_name(ilf_emotion_t e);

/* Размер экрана, чтобы переводить касание во взгляд. */
#define ILF_SCREEN_W 368
#define ILF_SCREEN_H 448

#ifdef __cplusplus
}
#endif
