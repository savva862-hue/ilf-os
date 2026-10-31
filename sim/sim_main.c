/*
 * Симулятор ILF OS на ПК: тот же код души и экрана, что и на плате,
 * только вместо AMOLED — картинка в памяти. Прогоняет сценки и сохраняет
 * скриншоты в PPM (скрипт sim/run.sh превращает их в PNG).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "ilf_soul.h"
#include "ilf_ui.h"

#define W ILF_SCREEN_W
#define H ILF_SCREEN_H

static uint32_t fb[W * H];
static uint32_t now_ms = 1000;
static ilf_soul_t soul;
static const char *out_dir = ".";

static uint32_t tick_cb(void) { return now_ms; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static void step(uint32_t ms)
{
    uint32_t end = now_ms + ms;
    while (now_ms < end) {
        now_ms += 33;
        ilf_soul_update(&soul, now_ms);
        ilf_ui_update_face(ilf_soul_face(&soul));
        lv_timer_handler();
    }
}

static void shot(const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint32_t c = fb[i]; /* ARGB8888 */
        unsigned char rgb[3] = { (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("saved %s (emotion: %s)\n", path, ilf_emotion_name(ilf_soul_face(&soul)->emotion));
}

static void on_touch(int16_t x, int16_t y, void *user)
{
    (void)user;
    ilf_event_t ev = { .type = ILF_EV_TOUCH, .touch = { x, y } };
    ilf_soul_handle(&soul, &ev, now_ms);
}

static void send(ilf_event_t ev) { ilf_soul_handle(&soul, &ev, now_ms); }

/* Дождаться момента, когда глаза открыты (не поймать кадр посреди моргания). */
static void settle(void)
{
    for (int i = 0; i < 100 && ilf_soul_face(&soul)->eyelid > 0.05f && ilf_soul_face(&soul)->emotion == ILF_EMO_CALM; i++) step(33);
}

int main(int argc, char **argv)
{
    if (argc > 1) out_dir = argv[1];

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(disp, fb, NULL, sizeof fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    ilf_soul_init(&soul, 1234, now_ms);
    ilf_ui_init();
    ilf_ui_set_pet_touch_cb(on_touch, NULL);
    ilf_ui_update_battery(83, false);

    struct tm t = {0};
    t.tm_year = 2026 - 1900; t.tm_mon = 9; t.tm_mday = 31; t.tm_hour = 14; t.tm_min = 42;
    t.tm_wday = 6; t.tm_yday = 303;
    ilf_ui_update_clock(&t);

    /* 1. Спокойный барашек */
    step(800); settle();
    shot("01_calm");

    /* 2. Коснулись справа — смотрит туда и радуется */
    on_touch(330, 200, NULL);
    step(400);
    shot("02_happy_touch");

    /* 3. Встряска — испугался */
    step(ILF_SOUL_HAPPY_MS);
    send((ilf_event_t){ .type = ILF_EV_SHAKE });
    step(300);
    shot("03_scared");

    /* 4. Трясли три раза подряд — обиделся */
    send((ilf_event_t){ .type = ILF_EV_SHAKE }); step(500);
    send((ilf_event_t){ .type = ILF_EV_SHAKE }); step(500);
    shot("04_sad");

    /* 5. Долго не трогали — засыпает, потом спит */
    step(ILF_SOUL_SAD_MS);
    step(ILF_SOUL_SLEEPY_AFTER_MS + 500);
    shot("05_sleepy");
    step(ILF_SOUL_ASLEEP_AFTER_MS - ILF_SOUL_SLEEPY_AFTER_MS);
    shot("06_asleep");

    /* 6. Голосовое от друга — просыпается и радуется */
    send((ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE });
    step(300);
    shot("07_friend_message");

    /* 7. Садится батарея — голоден */
    step(ILF_SOUL_FRIEND_HAPPY_MS + 200);
    send((ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { 12, false } });
    ilf_ui_update_battery(12, false);
    step(300);
    shot("08_hungry");
    send((ilf_event_t){ .type = ILF_EV_BATTERY, .battery = { 83, false } });
    ilf_ui_update_battery(83, false);
    step(300);

    /* 8. Остальные приложения (на плате — свайпом) */
    ilf_ui_show_app(ILF_APP_VOICE, false); step(100); shot("09_app_voice");
    ilf_ui_show_app(ILF_APP_DRAW, false);  step(100); shot("10_app_draw");
    ilf_ui_show_app(ILF_APP_MUSIC, false); step(100); shot("11_app_music");
    ilf_ui_show_app(ILF_APP_CLOCK, false); step(100); shot("12_app_clock");
    return 0;
}
