/*
 * Замок барашка: PIN из 4 цифр.
 *
 * Пока замок закрыт, виден только сам барашек; голосовые, рисунки, знакомство и
 * остальные приложения — после PIN. Открытый замок закрывается сам, когда экран
 * погас больше чем на ILF_LOCK_GRACE_MS. После ILF_LOCK_TRIES ошибок — пауза.
 * PIN хранится только как SHA-256(соль ‖ PIN), прокрученный ILF_LOCK_ROUNDS раз.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ILF_PIN_LEN         4
#define ILF_LOCK_TRIES      5
#define ILF_LOCK_WAIT_MS    30000u
#define ILF_LOCK_GRACE_MS   (5u * 60u * 1000u)
#define ILF_LOCK_ROUNDS     2000

typedef void (*ilf_sha256_fn)(const uint8_t *data, size_t len, uint8_t out[32]);

/* то, что хранится во flash */
typedef struct {
    uint8_t salt[16];
    uint8_t hash[32];
} ilf_pin_t;

typedef struct {
    ilf_pin_t pin;
    bool     has_pin;
    bool     unlocked;
    uint8_t  fails;
    uint32_t wait_until;
    uint32_t relock_at;     /* 0 — экран горит, закрываться не надо */
} ilf_lock_t;

typedef enum {
    ILF_LOCK_OK = 0,
    ILF_LOCK_WRONG,
    ILF_LOCK_WAIT,          /* слишком много ошибок, подожди ilf_lock_wait_left() */
} ilf_lock_result_t;

void ilf_lock_init(ilf_lock_t *l, const ilf_pin_t *stored);   /* stored == NULL — PIN ещё нет */
void ilf_lock_make_pin(ilf_pin_t *out, const char *pin, const uint8_t salt[16], ilf_sha256_fn sha);
ilf_lock_result_t ilf_lock_try(ilf_lock_t *l, const char *pin, uint32_t now_ms, ilf_sha256_fn sha);
uint32_t ilf_lock_wait_left(const ilf_lock_t *l, uint32_t now_ms);
bool ilf_lock_is_locked(ilf_lock_t *l, uint32_t now_ms);
void ilf_lock_screen_off(ilf_lock_t *l, uint32_t now_ms);
void ilf_lock_screen_on(ilf_lock_t *l, uint32_t now_ms);
void ilf_lock_now(ilf_lock_t *l);

#ifdef __cplusplus
}
#endif
