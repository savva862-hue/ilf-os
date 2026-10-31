#include "ilf_soul.h"

#include <stddef.h>

/* Сравнение времени, которое переживает переполнение uint32 (через ~49 дней). */
static inline bool time_reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static uint32_t rng_next(ilf_soul_t *s)
{
    /* xorshift32: детерминированно, чтобы тесты были повторяемыми */
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

static uint32_t rng_range(ilf_soul_t *s, uint32_t lo, uint32_t hi)
{
    return lo + rng_next(s) % (hi - lo + 1u);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void schedule_blink(ilf_soul_t *s)
{
    s->next_blink_ms = s->now_ms + rng_range(s, ILF_SOUL_BLINK_MIN_MS, ILF_SOUL_BLINK_MAX_MS);
}

static void set_transient(ilf_soul_t *s, ilf_emotion_t e, uint32_t duration_ms)
{
    s->transient = e;
    s->transient_until_ms = s->now_ms + duration_ms;
}

static void touch_activity(ilf_soul_t *s)
{
    s->last_activity_ms = s->now_ms;
}

void ilf_soul_init(ilf_soul_t *s, uint32_t seed, uint32_t now_ms)
{
    *s = (ilf_soul_t){0};
    s->rng = seed ? seed : 0x1F2E3D4Cu;
    s->now_ms = now_ms;
    s->last_activity_ms = now_ms;
    s->transient = ILF_EMO_CALM;
    s->transient_until_ms = now_ms;
    s->battery_percent = 100;
    s->next_wander_ms = now_ms + 1500u;
    schedule_blink(s);
    s->face.emotion = ILF_EMO_CALM;
}

static ilf_emotion_t base_emotion(const ilf_soul_t *s)
{
    if (s->battery_percent <= ILF_SOUL_HUNGRY_PERCENT && !s->charging) {
        return ILF_EMO_HUNGRY;
    }
    uint32_t idle = s->now_ms - s->last_activity_ms;
    if (idle >= ILF_SOUL_ASLEEP_AFTER_MS) return ILF_EMO_ASLEEP;
    if (idle >= ILF_SOUL_SLEEPY_AFTER_MS) return ILF_EMO_SLEEPY;
    return ILF_EMO_CALM;
}

static ilf_emotion_t current_emotion(const ilf_soul_t *s)
{
    if (!time_reached(s->now_ms, s->transient_until_ms)) {
        return s->transient;
    }
    return base_emotion(s);
}

void ilf_soul_handle(ilf_soul_t *s, const ilf_event_t *ev, uint32_t now_ms)
{
    if (!s || !ev) return;
    s->now_ms = now_ms;

    switch (ev->type) {
    case ILF_EV_TOUCH: {
        touch_activity(s);
        /* смотрит туда, где коснулись, и улыбается */
        s->target_gaze_x = clampf(((float)ev->touch.x - ILF_SCREEN_W / 2.0f) / (ILF_SCREEN_W / 2.0f), -1.f, 1.f);
        s->target_gaze_y = clampf(((float)ev->touch.y - ILF_SCREEN_H / 2.0f) / (ILF_SCREEN_H / 2.0f), -1.f, 1.f);
        s->look_until_ms = now_ms + ILF_SOUL_LOOK_AT_MS;
        if (current_emotion(s) != ILF_EMO_SAD) {
            set_transient(s, ILF_EMO_HAPPY, ILF_SOUL_HAPPY_MS);
        }
        break;
    }
    case ILF_EV_SHAKE: {
        touch_activity(s);
        s->shake_times[s->shake_idx] = now_ms;
        s->shake_idx = (uint8_t)((s->shake_idx + 1u) % ILF_SOUL_SHAKES_TO_SAD);
        if (s->shake_count < ILF_SOUL_SHAKES_TO_SAD) s->shake_count++;
        /* самая старая из последних N встрясок в пределах окна — значит, трясут часто */
        uint32_t oldest = s->shake_times[s->shake_idx];
        if (s->shake_count >= ILF_SOUL_SHAKES_TO_SAD && (now_ms - oldest) <= ILF_SOUL_SHAKE_WINDOW_MS) {
            set_transient(s, ILF_EMO_SAD, ILF_SOUL_SAD_MS);
        } else {
            set_transient(s, ILF_EMO_SCARED, ILF_SOUL_SCARED_MS);
        }
        break;
    }
    case ILF_EV_TILT: {
        int ax = ev->tilt.x < 0 ? -ev->tilt.x : ev->tilt.x;
        int ay = ev->tilt.y < 0 ? -ev->tilt.y : ev->tilt.y;
        if (ax > 300 || ay > 300) {
            touch_activity(s); /* заметный наклон = его взяли в руки */
        }
        if (time_reached(now_ms, s->look_until_ms)) {
            s->target_gaze_x = clampf(ev->tilt.x / 1000.0f, -1.f, 1.f);
            s->target_gaze_y = clampf(ev->tilt.y / 1000.0f, -1.f, 1.f);
            s->next_wander_ms = now_ms + 2000u;
        }
        break;
    }
    case ILF_EV_BUTTON:
        touch_activity(s);
        break;
    case ILF_EV_BATTERY:
        s->battery_percent = ev->battery.percent > 100 ? 100 : ev->battery.percent;
        s->charging = ev->battery.charging;
        break;
    case ILF_EV_FRIEND_MESSAGE:
        touch_activity(s);
        set_transient(s, ILF_EMO_HAPPY, ILF_SOUL_FRIEND_HAPPY_MS);
        break;
    }
    ilf_soul_update(s, now_ms);
}

static float mouth_for(ilf_emotion_t e)
{
    switch (e) {
    case ILF_EMO_HAPPY:  return 1.0f;
    case ILF_EMO_SAD:    return -0.8f;
    case ILF_EMO_HUNGRY: return -0.3f;
    case ILF_EMO_SLEEPY: return 0.0f;
    case ILF_EMO_ASLEEP: return 0.1f;
    case ILF_EMO_SCARED: return 0.0f;
    default:             return 0.2f;
    }
}

void ilf_soul_update(ilf_soul_t *s, uint32_t now_ms)
{
    s->now_ms = now_ms;
    ilf_emotion_t e = current_emotion(s);
    ilf_face_t *f = &s->face;
    f->emotion = e;

    /* --- моргание --- */
    if (!s->blinking && time_reached(now_ms, s->next_blink_ms)) {
        s->blinking = true;
        s->blink_started_ms = now_ms;
    }
    float blink = 0.f;
    if (s->blinking) {
        uint32_t t = now_ms - s->blink_started_ms;
        if (t >= ILF_SOUL_BLINK_LEN_MS) {
            s->blinking = false;
            schedule_blink(s);
        } else {
            /* закрыть и открыть: треугольник 0 → 1 → 0 */
            float p = (float)t / (float)ILF_SOUL_BLINK_LEN_MS;
            blink = p < 0.5f ? p * 2.f : (1.f - p) * 2.f;
        }
    }

    float lid_base;
    switch (e) {
    case ILF_EMO_ASLEEP: lid_base = 1.0f; break;
    case ILF_EMO_SLEEPY: lid_base = 0.55f; break;
    case ILF_EMO_SAD:    lid_base = 0.3f; break;
    case ILF_EMO_HUNGRY: lid_base = 0.2f; break;
    case ILF_EMO_SCARED: lid_base = 0.0f; blink = 0.f; break; /* испуганный не моргает */
    default:             lid_base = 0.0f; break;
    }
    f->eyelid = clampf(lid_base + blink * (1.f - lid_base), 0.f, 1.f);

    /* --- взгляд --- */
    if (e == ILF_EMO_CALM && time_reached(now_ms, s->look_until_ms) && time_reached(now_ms, s->next_wander_ms)) {
        /* спокойный барашек иногда поглядывает по сторонам */
        s->target_gaze_x = ((float)rng_range(s, 0, 100) - 50.f) / 100.f;
        s->target_gaze_y = ((float)rng_range(s, 0, 60) - 30.f) / 100.f;
        s->next_wander_ms = now_ms + rng_range(s, 1500, 4000);
    }
    /* на зарядке барашек жуёт клевер, если не спит и не расстроен */
    f->eating = s->charging && (e == ILF_EMO_CALM || e == ILF_EMO_HAPPY);
    if (f->eating) {
        float p = (float)(now_ms % ILF_SOUL_CHEW_MS) / (float)ILF_SOUL_CHEW_MS;
        f->chew = p < 0.5f ? p * 2.f : (1.f - p) * 2.f;
    } else {
        f->chew = 0.f;
    }

    float tx = s->target_gaze_x, ty = s->target_gaze_y;
    if (f->eating && time_reached(now_ms, s->look_until_ms)) { tx *= 0.3f; ty = 0.45f; } /* смотрит на клевер */
    if (e == ILF_EMO_ASLEEP || e == ILF_EMO_SLEEPY) { tx = 0.f; ty = 0.3f; }
    if (e == ILF_EMO_SAD) { ty = 0.6f; }
    f->gaze_x += (tx - f->gaze_x) * 0.25f;
    f->gaze_y += (ty - f->gaze_y) * 0.25f;

    /* --- рот --- */
    f->mouth = mouth_for(e);
    f->mouth_open = (e == ILF_EMO_SCARED) || (e == ILF_EMO_HUNGRY);
}

const ilf_face_t *ilf_soul_face(const ilf_soul_t *s) { return &s->face; }

bool ilf_soul_is_asleep(const ilf_soul_t *s) { return s->face.emotion == ILF_EMO_ASLEEP; }

const char *ilf_emotion_name(ilf_emotion_t e)
{
    static const char *names[ILF_EMO_COUNT] = {
        "calm", "happy", "sad", "scared", "sleepy", "asleep", "hungry",
    };
    return (unsigned)e < ILF_EMO_COUNT ? names[e] : "?";
}
