/*
 * Симулятор ILF OS на ПК: тот же код души и экрана, что и на плате,
 * только вместо AMOLED — картинка в памяти. Прогоняет сценки и сохраняет
 * скриншоты в PPM (скрипт sim/run.sh превращает их в PNG).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

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

/* виртуальный палец для сценок с рисованием */
static lv_point_t finger;
static bool finger_down;
static void finger_read(lv_indev_t *i, lv_indev_data_t *d)
{
    (void)i;
    d->point = finger;
    d->state = finger_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void tap(int x, int y)
{
    finger = (lv_point_t){ x, y }; finger_down = true; step(66);
    finger_down = false; step(66);
}
static void swipe(int x0, int x1, int y)
{
    finger = (lv_point_t){ x0, y }; finger_down = true; step(33);
    for (int i = 1; i <= 6; i++) { finger.x = x0 + (x1 - x0) * i / 6; step(17); }
    finger_down = false; step(100);
}
/* клавиатура имён: 4 ряда букв (по 4) + нижний ряд A… Q… ⌫ OK */
static void key(int row, int col) { tap(8 + 41 + col * 90, 162 + 25 + row * 58); }
static void kctl(int col) { key(4, col); }

static ilf_setup_result_t setup_res;
static bool setup_done;
static void on_setup(const ilf_setup_result_t *r, void *u) { (void)u; setup_res = *r; setup_done = true; }
static int strokes_sent, stroke_points;
static void on_stroke(const ilf_stroke_t *st, void *user) { (void)user; strokes_sent++; stroke_points += st->count; }

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
    lv_indev_t *finger_dev = lv_indev_create();
    lv_indev_set_type(finger_dev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger_dev, finger_read);
    ilf_ui_herd_cb_t cb = { .stroke = on_stroke };
    ilf_ui_set_herd_cb(&cb);
    ilf_ui_update_battery(83, false);

    struct tm t = {0};
    t.tm_year = 2026 - 1900; t.tm_mon = 9; t.tm_mday = 31; t.tm_hour = 14; t.tm_min = 42;
    t.tm_wday = 6; t.tm_yday = 303;
    ilf_ui_update_clock(&t);

    /* 0. Первый запуск: имя хозяина и имя барашка (латиница) */
    ilf_ui_setup_start(on_setup, NULL);
    step(100); shot("00a_setup_hello");
    tap(184, 448 - 28 - 30);                         /* «Дальше» */
    kctl(1); key(0, 2);                              /* S */
    kctl(0); key(0, 0); kctl(1); key(1, 1); key(1, 1); kctl(0); key(0, 0); /* a v v a */
    step(100); shot("00b_setup_owner");
    kctl(3);
    key(2, 0); key(2, 3); key(1, 1);                 /* I l f */
    step(100); shot("00c_setup_sheep");
    kctl(3);
    step(100); shot("00d_setup_done");
    tap(184, 448 - 20 - 30);                         /* «Начать» */
    printf("setup: done=%d owner=%s sheep=%s\n", setup_done, setup_res.owner, setup_res.sheep);
    ilf_ui_set_sheep_name(setup_res.sheep);

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

    /* 8. Знакомство стуком */
    ilf_ui_pet_note("Знакомимся…");
    step(300); settle();
    shot("09_pairing");
    ilf_ui_pet_note("Дружим с Барашек 3F2A!");
    send((ilf_event_t){ .type = ILF_EV_FRIEND_MESSAGE });
    step(300);
    shot("10_new_friend");
    ilf_ui_pet_note(NULL);

    /* 9. Голосовые: список и запись */
    ilf_ui_voice_item_t items[3] = {
        { "Барашек 3F2A", 7, true }, { "Барашек 91C0", 12, false }, { "Барашек 3F2A", 3, false },
    };
    ilf_ui_set_target("Барашек 3F2A");
    ilf_ui_voice_list(items, 3);
    ilf_ui_voice_state(ILF_VOICE_IDLE, 0);
    ilf_ui_show_app(ILF_APP_VOICE, false); step(100); shot("11_voice");
    ilf_ui_voice_state(ILF_VOICE_RECORDING, 4); step(100); shot("12_voice_recording");
    ilf_ui_voice_state(ILF_VOICE_SENDING, 0.45f); step(100); shot("13_voice_sending");

    /* 10. Рисунки: свой штрих пальцем (кремовый) и штрих друга (розовый) */
    ilf_ui_show_app(ILF_APP_DRAW, false); step(100);
    finger = (lv_point_t){ 70, 300 }; finger_down = true; step(66);
    for (int i = 0; i <= 30; i++) {
        finger.x = (int32_t)(70 + i * 7);
        finger.y = (int32_t)(300 - 90 * sin(i / 30.0 * 3.14159));
        step(33);
    }
    finger_down = false; step(66);
    ilf_stroke_t fr = { .stroke_id = 1, .count = 0 };
    for (int i = 0; i < 20; i++) {
        fr.pts[fr.count].x = (uint16_t)(160 + 60 * cos(i / 19.0 * 6.283));
        fr.pts[fr.count].y = (uint16_t)(120 + 60 * sin(i / 19.0 * 6.283));
        fr.count++;
    }
    ilf_ui_draw_remote(0, &fr);
    step(100);
    printf("draw: tile=%d strokes_sent=%d points=%d\n", (int)ilf_ui_current_app(), strokes_sent, stroke_points);
    shot("14_draw");

    ilf_ui_show_app(ILF_APP_PET, false); step(100);
    ilf_ui_show_app(ILF_APP_MUSIC, false); step(100); shot("15_app_music");
    ilf_ui_show_app(ILF_APP_CLOCK, false); step(100); shot("16_app_clock");
    return 0;
}
