/*
 * ILF OS — «Стадо»: закрытая сеть барашков рядом друг с другом.
 *
 * Что умеет:
 *   - знакомство стуком: два барашка стукнулись (гироскоп увидел удар в одно и то же
 *     мгновение) и обменялись ключами X25519. Чужой, кто не стукался, ключ не получит;
 *   - список друзей (до ILF_HERD_MAX_FRIENDS), его хранит приложение;
 *   - голосовые: сообщение режется на пакеты, доставка с повторами;
 *   - рисунки: штрихи летят другу сразу, пока рисуешь.
 *
 * Модуль на чистом C и не знает про радио. Всё железо приходит через ilf_herd_io_t:
 * на плате это ESP-NOW и mbedtls, в тестах — эмуляция эфира. Пакеты между друзьями
 * идут адресно и шифруются самим ESP-NOW (ключ LMK на каждого друга).
 *
 * Все функции зовутся из одного потока.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ILF_HERD_MAX_FRIENDS     12
#define ILF_HERD_NAME_LEN        24       /* UTF-8 с нулём: до 11 русских букв */
#define ILF_HERD_MAX_FRAME      250       /* предел одного пакета ESP-NOW */
#define ILF_HERD_CHUNK_DATA     229       /* = ILF_ADPCM_BLOCK_BYTES */
#define ILF_HERD_MAX_MESSAGE    (ILF_HERD_CHUNK_DATA * 300)  /* ~17 секунд голоса */
#define ILF_HERD_STROKE_POINTS   40       /* точек в одном пакете рисунка */
#define ILF_HERD_ALL            (-1)      /* адресат «всё стадо» */

/* Знакомство: насколько близко по времени должны совпасть удары и сколько ждать */
#define ILF_HERD_BUMP_MATCH_MS   450u
#define ILF_HERD_BUMP_WINDOW_MS 2000u
#define ILF_HERD_HELLO_REPEAT_MS 250u
#define ILF_HERD_HELLO_COUNT       4u
/* Доставка: повторы одного пакета и сколько ждать недостающие куски */
#define ILF_HERD_CHUNK_RETRIES     6u
#define ILF_HERD_RX_TIMEOUT_MS  4000u
/* Экономия батареи: радио друга без дела просыпается раз в ILF_NET_WAKE_INTERVAL (≈1 с).
   Первый пакет сообщения повторяем столько, чтобы попасть в его окно. */
#define ILF_HERD_WAKE_MS        2500u
#define ILF_HERD_WAKE_RETRY_MS    40u

typedef struct {
    uint8_t mac[6];
    uint8_t key[16];      /* LMK для ESP-NOW, выведен из общего секрета X25519 */
    char    name[ILF_HERD_NAME_LEN];
    uint32_t last_seen_ms;
    bool    seen;         /* слышали его с момента включения */
} ilf_friend_t;

typedef enum {
    ILF_MSG_VOICE = 1,    /* блоки IMA ADPCM, см. ilf_adpcm.h */
} ilf_msg_kind_t;

typedef struct {
    uint8_t  stroke_id;   /* новый id = новый штрих */
    bool     clear;       /* друг очистил холст */
    uint8_t  count;
    struct { uint16_t x, y; } pts[ILF_HERD_STROKE_POINTS];
} ilf_stroke_t;

typedef enum {
    ILF_HERD_EV_FRIEND_ADDED,   /* friend_idx: новый друг (или старый друг стукнулся снова) */
    ILF_HERD_EV_PAIR_TIMEOUT,   /* стукнулись, но второго барашка не услышали */
    ILF_HERD_EV_MESSAGE,        /* friend_idx, kind, data, len: пришло сообщение целиком */
    ILF_HERD_EV_STROKE,         /* friend_idx, stroke: кусок рисунка */
    ILF_HERD_EV_SENT,           /* friend_idx, msg_id, ok: доставка одному другу закончилась */
    ILF_HERD_EV_SAVE,           /* список друзей поменялся — сохранить (ilf_herd_friends) */
} ilf_herd_ev_type_t;

typedef struct {
    ilf_herd_ev_type_t type;
    int friend_idx;
    uint16_t msg_id;
    bool ok;
    ilf_msg_kind_t kind;
    const uint8_t *data;   /* живёт только до выхода из обработчика */
    size_t len;
    const ilf_stroke_t *stroke;
} ilf_herd_event_t;

typedef struct {
    void *ctx;
    /* Отправить пакет: mac == NULL — широковещательно (без шифрования). После каждой
       отправки платформа обязана позвать ilf_herd_on_sent() с результатом. */
    void (*send)(void *ctx, const uint8_t *mac, const uint8_t *data, size_t len);
    /* Зарегистрировать друга в радио с ключом шифрования. */
    void (*add_peer)(void *ctx, const uint8_t mac[6], const uint8_t key[16]);
    void (*remove_peer)(void *ctx, const uint8_t mac[6]);
    /* Криптография: X25519 и SHA-256. */
    void (*keypair)(void *ctx, uint8_t priv[32], uint8_t pub[32]);
    bool (*shared)(void *ctx, const uint8_t priv[32], const uint8_t peer_pub[32], uint8_t out[32]);
    void (*sha256)(void *ctx, const uint8_t *data, size_t len, uint8_t out[32]);
    void (*event)(void *ctx, const ilf_herd_event_t *ev);
} ilf_herd_io_t;

typedef struct ilf_herd ilf_herd_t;

/* Создать стадо. my_name — как этого барашка увидят друзья. */
ilf_herd_t *ilf_herd_create(const ilf_herd_io_t *io, const uint8_t my_mac[6], const char *my_name);
void ilf_herd_destroy(ilf_herd_t *h);
/* Как этого барашка видят друзья (имя хозяина). Новое имя друзья узнают при следующем знакомстве. */
void ilf_herd_set_name(ilf_herd_t *h, const char *name);

/* Вернуть сохранённых друзей (при включении). Каждого регистрирует в радио. */
void ilf_herd_load_friends(ilf_herd_t *h, const ilf_friend_t *friends, int count);
int  ilf_herd_friend_count(const ilf_herd_t *h);
const ilf_friend_t *ilf_herd_friend(const ilf_herd_t *h, int idx);
int  ilf_herd_find_friend(const ilf_herd_t *h, const uint8_t mac[6]);
void ilf_herd_forget_friend(ilf_herd_t *h, int idx);

/* Гироскоп увидел удар — начать знакомство. */
void ilf_herd_bump(ilf_herd_t *h, uint32_t now_ms);
bool ilf_herd_pairing(const ilf_herd_t *h);

/* Отправить сообщение другу (или ILF_HERD_ALL). Данные копируются.
   Возвращает id сообщения или 0, если очередь занята или нет друзей. */
uint16_t ilf_herd_send(ilf_herd_t *h, int friend_idx, ilf_msg_kind_t kind,
                       const uint8_t *data, size_t len, uint32_t now_ms);
bool ilf_herd_sending(const ilf_herd_t *h);
/* Ход отправки 0..1 */
float ilf_herd_send_progress(const ilf_herd_t *h);

/* Кусок рисунка — без повторов, теряться не страшно. */
void ilf_herd_send_stroke(ilf_herd_t *h, int friend_idx, const ilf_stroke_t *s, uint32_t now_ms);

/* От радио: пришёл пакет / закончилась отправка. */
void ilf_herd_on_recv(ilf_herd_t *h, const uint8_t mac[6], const uint8_t *data, size_t len, uint32_t now_ms);
void ilf_herd_on_sent(ilf_herd_t *h, bool ok, uint32_t now_ms);

/* Таймеры: повтор приветствий, тайм-ауты. Звать раз в 10–50 мс. */
void ilf_herd_tick(ilf_herd_t *h, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
