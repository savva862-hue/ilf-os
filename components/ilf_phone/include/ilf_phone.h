/*
 * iPhone без приложения: что барашок понимает из стандартных служб Bluetooth LE.
 *
 *   CTS  (Current Time Service, 0x1805)  — точное время и часовой пояс телефона;
 *   AMS  (Apple Media Service)           — что играет и пульт: пауза, дальше, назад;
 *   ANCS (Apple Notification Center)     — уведомления телефона.
 *
 * Здесь только разбор байтов и сборка команд. Само радио — main/ilf_ble.c (NimBLE).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- CTS ---------- */

/* Current Time (0x2A2B), 10 байт. Время местное, как на телефоне. */
bool ilf_cts_parse_time(const uint8_t *d, size_t len, struct tm *local);
/* Local Time Information (0x2A0F), 2 байта → строка TZ для setenv("TZ"), например "<+09>-9".
   Возвращает смещение от UTC в минутах. */
bool ilf_cts_parse_zone(const uint8_t *d, size_t len, char *tz, size_t tz_len, int *offset_min);

/* ---------- AMS ---------- */

typedef enum {
    ILF_AMS_PLAY = 0, ILF_AMS_PAUSE = 1, ILF_AMS_TOGGLE = 2,
    ILF_AMS_NEXT = 3, ILF_AMS_PREV = 4, ILF_AMS_VOL_UP = 5, ILF_AMS_VOL_DOWN = 6,
} ilf_ams_cmd_t;

#define ILF_PHONE_TEXT 64

typedef struct {
    char artist[ILF_PHONE_TEXT];
    char title[ILF_PHONE_TEXT];
    bool playing;
    bool changed;   /* выставляет разбор, сбрасывает тот, кто перерисовал */
} ilf_ams_state_t;

/* Подписка на нужные поля: пишется в Entity Update по одной сущности за раз. */
size_t ilf_ams_subscribe_track(uint8_t *out);    /* артист и название */
size_t ilf_ams_subscribe_player(uint8_t *out);   /* играет / пауза */
/* Уведомление Entity Update → состояние. */
bool ilf_ams_parse(ilf_ams_state_t *st, const uint8_t *d, size_t len);

/* ---------- ANCS ---------- */

typedef enum {
    ILF_ANCS_ADDED = 0, ILF_ANCS_MODIFIED = 1, ILF_ANCS_REMOVED = 2,
} ilf_ancs_event_t;

typedef struct {
    ilf_ancs_event_t event;
    uint8_t  category;     /* 1 — входящий звонок, 4 — сообщения, … */
    bool     silent;
    bool     preexisting;  /* висело ещё до подключения */
    uint32_t uid;
} ilf_ancs_source_t;

typedef struct {
    uint32_t uid;
    char app[ILF_PHONE_TEXT];      /* com.apple.MobileSMS */
    char title[ILF_PHONE_TEXT];    /* кто */
    char message[128];             /* что */
} ilf_ancs_note_t;

/* Notification Source, 8 байт. */
bool ilf_ancs_parse_source(const uint8_t *d, size_t len, ilf_ancs_source_t *out);
/* Команда в Control Point: «дай приложение, заголовок и текст уведомления uid». */
size_t ilf_ancs_get_attributes(uint32_t uid, uint8_t *out, size_t cap);

/* Команда в Control Point: нажать на уведомлении «да» (принять звонок) или «нет» (отклонить). 6 байт. */
size_t ilf_ancs_action(uint32_t uid, bool positive, uint8_t *out);

/* Ответ приходит в Data Source кусками — собираем, пока не будет целиком. */
typedef struct {
    uint8_t buf[512];
    size_t  len;
} ilf_ancs_rx_t;
void ilf_ancs_rx_reset(ilf_ancs_rx_t *rx);
/* true — уведомление собрано целиком и лежит в out. */
bool ilf_ancs_rx_feed(ilf_ancs_rx_t *rx, const uint8_t *d, size_t len, ilf_ancs_note_t *out);

/* Понятное имя приложения по bundle id: «Сообщения», «Telegram», … */
const char *ilf_phone_app_name(const char *bundle_id);

#ifdef __cplusplus
}
#endif
