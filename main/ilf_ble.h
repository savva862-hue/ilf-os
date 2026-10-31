/*
 * iPhone по Bluetooth LE без приложения (NimBLE).
 *
 * Барашек — периферия, как Apple Watch: появляется в «Настройки » Bluetooth»,
 * при первом подключении показывает на экране 6 цифр, их вводят на iPhone.
 * Дальше телефон переподключается сам. Помним только один iPhone — новый
 * вытесняет старый.
 *
 * С телефона берём: время и часовой пояс (CTS), что играет + пульт (AMS),
 * уведомления (ANCS; звонки пропускаем). Всё приходит событиями через ilf_ble_poll()
 * в поток экрана.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ilf_phone.h"

typedef enum {
    ILF_BLE_OFF = 0,
    ILF_BLE_WAITING,     /* рекламируемся, iPhone не подключён */
    ILF_BLE_PAIRING,     /* на экране код */
    ILF_BLE_CONNECTED,   /* зашифровано, службы найдены */
} ilf_ble_state_t;

typedef enum {
    ILF_BLE_EV_STATE,        /* state */
    ILF_BLE_EV_PASSKEY,      /* passkey — показать на экране */
    ILF_BLE_EV_TIME,         /* часы выставлены; tz — новая строка TZ (может быть пустой) */
    ILF_BLE_EV_MUSIC,        /* music */
    ILF_BLE_EV_NOTE,         /* note + category */
    ILF_BLE_EV_NOTE_GONE,    /* uid — уведомление убрали на телефоне (звонок кончился) */
} ilf_ble_ev_type_t;

#define ILF_ANCS_CAT_CALL 1

typedef struct {
    ilf_ble_ev_type_t type;
    union {
        ilf_ble_state_t state;
        uint32_t passkey;
        char tz[32];
        ilf_ams_state_t music;
        struct { ilf_ancs_note_t note; uint8_t category; };
        uint32_t uid;
    };
} ilf_ble_event_t;

/* Поднять Bluetooth и начать рекламу под именем барашка. */
void ilf_ble_init(const char *name);
void ilf_ble_set_name(const char *name);
/* Забрать очередное событие (из потока экрана). */
bool ilf_ble_poll(ilf_ble_event_t *ev);
bool ilf_ble_connected(void);

/* Пульт музыки. */
void ilf_ble_music(ilf_ams_cmd_t cmd);
/* Забыть iPhone (сброс к заводским). */
void ilf_ble_forget(void);
