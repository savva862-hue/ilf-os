/* Стадо на плате: ESP-NOW, X25519/SHA-256 из mbedtls, друзья в NVS. */
#pragma once

#include "ilf_herd.h"

typedef void (*ilf_net_event_fn)(const ilf_herd_event_t *ev, void *user);

/* Поднять радио и стадо. События приходят из ilf_net_poll(), то есть в потоке, который его зовёт. */
ilf_herd_t *ilf_net_init(ilf_net_event_fn fn, void *user);
/* Разобрать пришедшие пакеты и таймеры. Звать часто из одного потока. */
void ilf_net_poll(uint32_t now_ms);
const char *ilf_net_my_name(void);

/* Экономия батареи. Пока экран горит — радио слушает всё время. Экран погас —
   радио просыпается на ILF_NET_WAKE_WINDOW мс раз в ILF_NET_WAKE_INTERVAL мс
   (≈5% времени), а пришёл пакет или идёт отправка — снова слушает целиком. */
#define ILF_NET_WAKE_INTERVAL 1000
#define ILF_NET_WAKE_WINDOW     60
#define ILF_NET_STAY_AWAKE_MS 10000u
void ilf_net_keep_listening(bool on);
