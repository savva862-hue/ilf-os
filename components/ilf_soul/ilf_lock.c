#include "ilf_lock.h"

#include <string.h>

static int32_t diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void ilf_lock_init(ilf_lock_t *l, const ilf_pin_t *stored)
{
    memset(l, 0, sizeof *l);
    if (stored) {
        l->pin = *stored;
        l->has_pin = true;
    } else {
        l->unlocked = true;   /* без PIN закрывать нечем */
    }
}

static void hash_pin(const char *pin, const uint8_t salt[16], ilf_sha256_fn sha, uint8_t out[32])
{
    uint8_t buf[32 + 16 + ILF_PIN_LEN];
    memset(buf, 0, 32);
    memcpy(buf + 32, salt, 16);
    memcpy(buf + 48, pin, ILF_PIN_LEN);
    for (int i = 0; i < ILF_LOCK_ROUNDS; i++) {
        sha(buf, sizeof buf, out);
        memcpy(buf, out, 32);
    }
}

static bool valid_pin(const char *pin)
{
    if (!pin || strlen(pin) != ILF_PIN_LEN) return false;
    for (int i = 0; i < ILF_PIN_LEN; i++) if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}

void ilf_lock_make_pin(ilf_pin_t *out, const char *pin, const uint8_t salt[16], ilf_sha256_fn sha)
{
    memcpy(out->salt, salt, 16);
    hash_pin(pin, salt, sha, out->hash);
}

uint32_t ilf_lock_wait_left(const ilf_lock_t *l, uint32_t now)
{
    int32_t d = diff(l->wait_until, now);
    return l->wait_until && d > 0 ? (uint32_t)d : 0;
}

ilf_lock_result_t ilf_lock_try(ilf_lock_t *l, const char *pin, uint32_t now, ilf_sha256_fn sha)
{
    if (!l->has_pin) { l->unlocked = true; return ILF_LOCK_OK; }
    if (ilf_lock_wait_left(l, now)) return ILF_LOCK_WAIT;
    uint8_t h[32];
    bool ok = valid_pin(pin);
    if (ok) {
        hash_pin(pin, l->pin.salt, sha, h);
        uint8_t d = 0;
        for (int i = 0; i < 32; i++) d |= (uint8_t)(h[i] ^ l->pin.hash[i]);
        ok = d == 0;
    }
    if (ok) {
        l->unlocked = true;
        l->fails = 0;
        l->wait_until = 0;
        l->relock_at = 0;
        return ILF_LOCK_OK;
    }
    if (++l->fails >= ILF_LOCK_TRIES) {
        l->fails = 0;
        l->wait_until = now + ILF_LOCK_WAIT_MS;
        if (!l->wait_until) l->wait_until = 1;
        return ILF_LOCK_WAIT;
    }
    return ILF_LOCK_WRONG;
}

bool ilf_lock_is_locked(ilf_lock_t *l, uint32_t now)
{
    if (!l->has_pin) return false;
    if (l->unlocked && l->relock_at && diff(now, l->relock_at) >= 0) {
        l->unlocked = false;
        l->relock_at = 0;
    }
    return !l->unlocked;
}

void ilf_lock_screen_off(ilf_lock_t *l, uint32_t now)
{
    if (l->unlocked && !l->relock_at) {
        l->relock_at = now + ILF_LOCK_GRACE_MS;
        if (!l->relock_at) l->relock_at = 1;
    }
}

void ilf_lock_screen_on(ilf_lock_t *l, uint32_t now)
{
    ilf_lock_is_locked(l, now);   /* закрыть, если экран был погашен слишком долго */
    l->relock_at = 0;
}

void ilf_lock_now(ilf_lock_t *l)
{
    if (l->has_pin) l->unlocked = false;
    l->relock_at = 0;
}
