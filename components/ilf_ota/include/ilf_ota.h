/*
 * Обновление прошивки от барашка к барашку (по ESP-NOW, только между друзьями).
 *
 * Встретились два друга — каждый говорит другому свою версию прошивки (VER).
 * У кого новее, тот раздаёт. Получатель сначала спрашивает хозяина «обновить?»,
 * потом сам тянет образ кусками: REQ «дай с такого-то места N кусков» → DATA.
 * Потерялся кусок — через паузу просит снова с того же места. В конце
 * сверяет SHA-256 всего образа, и только тогда барашек перезагружается в новую версию.
 *
 * Кадры: [0xF2][тип][...], все числа little-endian.
 *   VER  1: u16 версия, u32 размер, 32 байта SHA-256 образа
 *   REQ  2: u32 смещение, u8 сколько кусков
 *   DATA 3: u32 смещение, до ILF_OTA_CHUNK байт образа
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ILF_OTA_MAGIC       0xF2
#define ILF_OTA_CHUNK       240
#define ILF_OTA_MAX_FRAME   (6 + ILF_OTA_CHUNK)
#define ILF_OTA_WINDOW      16      /* кусков на один запрос */
#define ILF_OTA_RETRY_MS    400     /* тишина — переспросить */
#define ILF_OTA_MAX_RETRIES 30      /* столько подряд без ответа — сдаться */
#define ILF_OTA_ANNOUNCE_MS (10u * 60u * 1000u)  /* версию одному другу — не чаще раза в 10 минут */
#define ILF_OTA_SEND_PER_TICK 8
#define ILF_OTA_PEERS       12

typedef struct {
    uint16_t version;
    uint32_t size;
    uint8_t  sha[32];
} ilf_ota_image_t;

typedef enum {
    ILF_OTA_EV_OFFER,      /* у друга новее: спросить хозяина (mac, version) */
    ILF_OTA_EV_PROGRESS,   /* progress 0..1 */
    ILF_OTA_EV_DONE,       /* образ записан и проверен — можно перезагружаться */
    ILF_OTA_EV_FAILED,     /* друг ушёл или образ битый */
} ilf_ota_ev_type_t;

typedef struct {
    ilf_ota_ev_type_t type;
    uint8_t  mac[6];
    uint16_t version;
    float    progress;
} ilf_ota_event_t;

typedef struct {
    /* false — радио занято, попробуем в следующий тик */
    bool (*send)(void *u, const uint8_t mac[6], const uint8_t *d, size_t n);
    /* своя прошивка: прочитать кусок, чтобы отдать другу */
    bool (*read)(void *u, uint32_t off, uint8_t *buf, size_t n);
    /* новая прошивка: начать, писать по порядку, закончить с проверкой SHA-256 */
    bool (*begin)(void *u, uint32_t size);
    bool (*write)(void *u, const uint8_t *d, size_t n);
    bool (*finish)(void *u, const uint8_t sha[32]);
    void (*abort)(void *u);
    void (*event)(void *u, const ilf_ota_event_t *ev);
    void *user;
} ilf_ota_io_t;

typedef enum { ILF_OTA_IDLE, ILF_OTA_OFFERED, ILF_OTA_PULLING, ILF_OTA_READY } ilf_ota_state_t;

typedef struct {
    ilf_ota_io_t io;
    ilf_ota_image_t mine;
    ilf_ota_state_t state;
    /* приём */
    uint8_t  src[6];
    ilf_ota_image_t want;
    uint16_t declined;           /* от этой версии хозяин отказался */
    uint32_t next;               /* сколько уже записано */
    uint32_t window_end;
    uint32_t last_rx_ms;
    int      retries;
    float    last_progress;
    /* раздача (одному другу за раз) */
    bool     serving;
    uint8_t  dst[6];
    uint32_t serve_off, serve_end;
    /* кому и когда говорили версию */
    struct { uint8_t mac[6]; uint32_t at; bool used; } told[ILF_OTA_PEERS];
} ilf_ota_t;

void ilf_ota_init(ilf_ota_t *o, const ilf_ota_io_t *io, const ilf_ota_image_t *mine);
/* друг рядом (пришёл любой его пакет) — сказать ему свою версию, если давно не говорили */
void ilf_ota_seen(ilf_ota_t *o, const uint8_t mac[6], uint32_t now);
/* пришёл кадр с ILF_OTA_MAGIC от друга */
void ilf_ota_on_recv(ilf_ota_t *o, const uint8_t mac[6], const uint8_t *d, size_t n, uint32_t now);
void ilf_ota_tick(ilf_ota_t *o, uint32_t now);
/* ответ хозяина на OFFER */
void ilf_ota_accept(ilf_ota_t *o, uint32_t now);
/* forever — «нет» этой версии насовсем; иначе «не сейчас»: друг предложит снова через 10 минут */
void ilf_ota_decline(ilf_ota_t *o, bool forever);
/* идёт приём или раздача — радио не усыплять */
bool ilf_ota_busy(const ilf_ota_t *o);

#ifdef __cplusplus
}
#endif
