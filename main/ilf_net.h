/* Стадо на плате: ESP-NOW, X25519/SHA-256 из mbedtls, друзья в NVS. */
#pragma once

#include "ilf_herd.h"

typedef void (*ilf_net_event_fn)(const ilf_herd_event_t *ev, void *user);

/* Поднять радио и стадо. События приходят из ilf_net_poll(), то есть в потоке, который его зовёт. */
ilf_herd_t *ilf_net_init(ilf_net_event_fn fn, void *user);
/* Разобрать пришедшие пакеты и таймеры. Звать часто из одного потока. */
void ilf_net_poll(uint32_t now_ms);
const char *ilf_net_my_name(void);
