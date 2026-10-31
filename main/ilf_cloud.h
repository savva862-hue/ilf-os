/*
 * Домашний Wi-Fi и Яндекс Диск.
 *
 * Барашек живёт на ESP-NOW (канал 1) и к Wi-Fi подключается только ненадолго:
 * найти сети, проверить пароль, выгрузить голосовые и рисунки на Диск. Потом
 * отключается и возвращается к стаду. Всё сетевое — в своей задаче, экран
 * не ждёт; результаты забираются через ilf_cloud_poll() в потоке экрана.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ILF_CLOUD_SSIDS 8

typedef enum {
    ILF_CLOUD_EV_SCAN,       /* ssids/n — найденные сети */
    ILF_CLOUD_EV_WIFI,       /* text — строка для «Wi-Fi» в настройках */
    ILF_CLOUD_EV_DISK_CODE,  /* url/code — показать; url пустой — убрать */
    ILF_CLOUD_EV_DISK,       /* text — строка для «Яндекс Диск» */
    ILF_CLOUD_EV_SYNCED,     /* выгрузка закончилась, список голосовых мог поменяться */
} ilf_cloud_ev_type_t;

typedef struct {
    ilf_cloud_ev_type_t type;
    char text[64];
    char ssids[ILF_CLOUD_SSIDS][33];
    int  n;
    char url[48];
    char code[24];
} ilf_cloud_event_t;

/* После ilf_net_init. device_name — как барашок назовётся в Яндексе. */
void ilf_cloud_init(const char *device_name);
bool ilf_cloud_poll(ilf_cloud_event_t *ev);

void ilf_cloud_scan(void);
void ilf_cloud_join(const char *ssid, const char *pass);
void ilf_cloud_forget(void);
/* Тап по «Яндекс Диск»: не привязан — привязать, привязан — выгрузить сейчас. */
void ilf_cloud_disk_tap(void);
/* Выгрузить, если есть что и есть Wi-Fi с Диском. Зовёт main, когда барашек свободен. */
void ilf_cloud_sync(void);
bool ilf_cloud_ready(void);   /* Wi-Fi задан и Диск привязан */
bool ilf_cloud_busy(void);    /* сейчас на Wi-Fi: стадо не слышно */
/* Обновить строки в настройках (после старта или изменения числа файлов). */
void ilf_cloud_refresh_status(void);
