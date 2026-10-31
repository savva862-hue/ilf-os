#include "ilf_ui.h"

#include <stdio.h>

#include "ilf_clock.h"

LV_FONT_DECLARE(ilf_font_24);
LV_FONT_DECLARE(ilf_font_32);
LV_FONT_DECLARE(ilf_font_48);
LV_FONT_DECLARE(ilf_font_digits_120);

/* Палитра: AMOLED-чёрный фон почти не тратит батарею */
#define C_BG       0x000000
#define C_WOOL     0xF5EFE4
#define C_FACE     0x3A3442
#define C_EYE      0xF5EFE4
#define C_PUPIL    0x121016
#define C_NOSE     0xF2A7B5
#define C_GOLD     0xF2B84B
#define C_MUTED    0x8F879B
#define C_CARD     0x1E1A24
#define C_TEAR     0x7EC8F2

/* Геометрия лица (экран 368×448) */
#define FACE_CX 184
#define FACE_CY 252
#define EYE_W 36
#define EYE_H 44
#define PUPIL 20
#define EYE_L_CX 156
#define EYE_R_CX 212
#define EYE_CY 236

typedef struct {
    lv_obj_t *eye;
    lv_obj_t *pupil;
    lv_obj_t *shine;
    lv_obj_t *lid;
    lv_obj_t *closed; /* закрытый глаз: дужка «︶» */
} eye_t;

static struct {
    lv_obj_t *tileview;
    lv_obj_t *tiles[ILF_APP_COUNT];
    lv_obj_t *dots[ILF_APP_COUNT];
    /* питомец */
    lv_obj_t *stage;
    eye_t eyes[2];
    lv_obj_t *smile;     /* дуга вниз */
    lv_obj_t *frown;     /* дуга вверх */
    lv_obj_t *flat;      /* ровный рот */
    lv_obj_t *mouth_o;   /* открытый рот */
    lv_obj_t *zzz;
    lv_obj_t *tear;
    lv_obj_t *sweat;
    lv_obj_t *status;
    ilf_ui_touch_cb_t touch_cb;
    void *touch_user;
    /* часы */
    lv_obj_t *time_lbl;
    lv_obj_t *date_lbl;
    lv_obj_t *year_bar;
    lv_obj_t *year_lbl;
    lv_obj_t *battery_lbl;
    ilf_emotion_t last_emotion;
} ui;

/* ---------- маленькие помощники ---------- */

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* Залитый эллипс с центром в (cx, cy). */
static lv_obj_t *blob(lv_obj_t *parent, int cx, int cy, int w, int h, uint32_t color)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *mouth_arc(lv_obj_t *parent, int cx, int cy, int size, int start, int end)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_size(a, size, size);
    lv_obj_set_pos(a, cx - size / 2, cy - size / 2);
    lv_arc_set_bg_angles(a, start, end);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_color(a, lv_color_hex(C_WOOL), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(a, 0, 0);
    return a;
}

static void show(lv_obj_t *o, bool visible)
{
    if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *app_title(lv_obj_t *tile, const char *title, const char *subtitle, const char *soon)
{
    lv_obj_t *t = label(tile, &ilf_font_48, C_WOOL, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_t *s = label(tile, &ilf_font_24, C_MUTED, subtitle);
    lv_obj_set_width(s, 300);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s, t, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
    if (soon) {
        lv_obj_t *b = label(tile, &ilf_font_24, C_GOLD, soon);
        lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -52);
    }
    return t;
}

/* ---------- экран «Питомец» ---------- */

static void pet_clicked(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !ui.touch_cb) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    ui.touch_cb((int16_t)p.x, (int16_t)p.y, ui.touch_user);
}

static void make_eye(eye_t *eye, lv_obj_t *parent, int cx)
{
    eye->eye = blob(parent, cx, EYE_CY, EYE_W, EYE_H, C_EYE);
    lv_obj_set_style_clip_corner(eye->eye, true, 0); /* веко и зрачок не вылезают за глаз */
    eye->pupil = blob(eye->eye, EYE_W / 2, EYE_H / 2 + 4, PUPIL, PUPIL, C_PUPIL);
    eye->shine = blob(eye->eye, EYE_W / 2 + 4, EYE_H / 2, 6, 6, C_EYE);
    eye->lid = plain(eye->eye);
    lv_obj_set_style_bg_color(eye->lid, lv_color_hex(C_FACE), 0);
    lv_obj_set_style_bg_opa(eye->lid, LV_OPA_COVER, 0);
    lv_obj_set_size(eye->lid, EYE_W, 0);
    lv_obj_set_pos(eye->lid, 0, 0);
    eye->closed = mouth_arc(parent, cx, EYE_CY - 6, 30, 20, 160);
    lv_obj_add_flag(eye->closed, LV_OBJ_FLAG_HIDDEN);
}

static void build_pet(lv_obj_t *tile)
{
    ui.stage = plain(tile);
    lv_obj_set_size(ui.stage, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_add_flag(ui.stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.stage, pet_clicked, LV_EVENT_CLICKED, NULL);

    /* шерсть: облако из кружков */
    static const int16_t wool[][3] = {
        {184, 150, 58}, {124, 176, 50}, {244, 176, 50}, {104, 236, 46}, {264, 236, 46},
        {130, 296, 48}, {238, 296, 48}, {184, 316, 56}, {184, 226, 70},
    };
    for (size_t i = 0; i < sizeof wool / sizeof wool[0]; i++) {
        blob(ui.stage, wool[i][0], wool[i][1], wool[i][2] * 2, wool[i][2] * 2, C_WOOL);
    }
    /* уши и морда */
    blob(ui.stage, 98, 218, 76, 36, C_FACE);
    blob(ui.stage, 270, 218, 76, 36, C_FACE);
    blob(ui.stage, FACE_CX, FACE_CY, 144, 176, C_FACE);

    make_eye(&ui.eyes[0], ui.stage, EYE_L_CX);
    make_eye(&ui.eyes[1], ui.stage, EYE_R_CX);

    blob(ui.stage, FACE_CX, 292, 32, 20, C_NOSE);

    ui.smile = mouth_arc(ui.stage, FACE_CX, 300, 36, 30, 150);
    ui.frown = mouth_arc(ui.stage, FACE_CX, 334, 36, 210, 330);
    ui.flat = plain(ui.stage);
    lv_obj_set_size(ui.flat, 26, 5);
    lv_obj_set_pos(ui.flat, FACE_CX - 13, 314);
    lv_obj_set_style_radius(ui.flat, 3, 0);
    lv_obj_set_style_bg_color(ui.flat, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_bg_opa(ui.flat, LV_OPA_COVER, 0);
    ui.mouth_o = blob(ui.stage, FACE_CX, 318, 18, 22, C_PUPIL);
    lv_obj_set_style_border_color(ui.mouth_o, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_border_width(ui.mouth_o, 3, 0);

    ui.tear = blob(ui.stage, 140, 272, 10, 16, C_TEAR);
    ui.sweat = blob(ui.stage, 250, 200, 12, 18, C_TEAR);
    ui.zzz = label(ui.stage, &ilf_font_32, C_GOLD, "z z Z");
    lv_obj_set_pos(ui.zzz, 250, 96);

    ui.status = label(ui.stage, &ilf_font_24, C_MUTED, "");
    lv_obj_set_width(ui.status, ILF_SCREEN_W);
    lv_obj_set_style_text_align(ui.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui.status, 0, 380);

    ui.last_emotion = ILF_EMO_COUNT;
}

void ilf_ui_update_face(const ilf_face_t *f)
{
    if (!ui.stage || !f) return;

    for (int i = 0; i < 2; i++) {
        eye_t *e = &ui.eyes[i];
        int px = EYE_W / 2 + (int)(f->gaze_x * 7.0f);
        int py = EYE_H / 2 + (int)(f->gaze_y * 9.0f);
        lv_obj_set_pos(e->pupil, px - PUPIL / 2, py - PUPIL / 2);
        lv_obj_set_pos(e->shine, px + 1, py - 6);
        lv_obj_set_height(e->lid, (int32_t)(f->eyelid * EYE_H + 0.5f));
        bool closed = f->eyelid > 0.92f; /* полностью закрытый глаз рисуем дужкой, без контура */
        show(e->eye, !closed);
        show(e->closed, closed);
    }

    show(ui.smile, !f->mouth_open && f->mouth > 0.5f);
    show(ui.frown, !f->mouth_open && f->mouth < -0.5f);
    show(ui.flat, !f->mouth_open && f->mouth >= -0.5f && f->mouth <= 0.5f);
    show(ui.mouth_o, f->mouth_open);

    if (f->emotion != ui.last_emotion) {
        ui.last_emotion = f->emotion;
        show(ui.zzz, f->emotion == ILF_EMO_ASLEEP || f->emotion == ILF_EMO_SLEEPY);
        show(ui.tear, f->emotion == ILF_EMO_SAD);
        show(ui.sweat, f->emotion == ILF_EMO_SCARED);
        const char *txt = "";
        switch (f->emotion) {
        case ILF_EMO_HUNGRY: txt = "Хочу кушать!"; break;
        case ILF_EMO_SAD:    txt = "Не тряси меня так"; break;
        default: break;
        }
        lv_label_set_text(ui.status, txt);
    }
}

void ilf_ui_set_pet_touch_cb(ilf_ui_touch_cb_t cb, void *user)
{
    ui.touch_cb = cb;
    ui.touch_user = user;
}

/* ---------- остальные приложения ---------- */

static void build_voice(lv_obj_t *tile)
{
    app_title(tile, "Голосовые", "Зажми кнопку и скажи другу из стада", "скоро · v0.2");
    lv_obj_t *mic = blob(tile, ILF_SCREEN_W / 2, 290, 120, 120, C_CARD);
    lv_obj_set_style_border_color(mic, lv_color_hex(C_GOLD), 0);
    lv_obj_set_style_border_width(mic, 4, 0);
    lv_obj_t *dot = blob(mic, 60, 60, 36, 36, C_GOLD);
    (void)dot;
}

static void build_draw(lv_obj_t *tile)
{
    app_title(tile, "Рисунки", "Рисуй пальцем, друг увидит штрих за штрихом", "скоро · v0.2");
    lv_obj_t *canvas = plain(tile);
    lv_obj_set_size(canvas, 240, 130);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 62);
    lv_obj_set_style_radius(canvas, 24, 0);
    lv_obj_set_style_border_color(canvas, lv_color_hex(C_NOSE), 0);
    lv_obj_set_style_border_width(canvas, 3, 0);
}

static void build_music(lv_obj_t *tile)
{
    app_title(tile, "Музыка", "Пульт того, что играет на iPhone", "скоро · v0.3");
    static const char *syms[3] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    for (int i = 0; i < 3; i++) {
        int size = i == 1 ? 104 : 80;
        lv_obj_t *b = blob(tile, 84 + i * 100, 280, size, size, i == 1 ? C_GOLD : C_CARD);
        lv_obj_t *l = label(b, &lv_font_montserrat_32, i == 1 ? C_PUPIL : C_WOOL, syms[i]);
        lv_obj_center(l);
    }
}

static void build_clock(lv_obj_t *tile)
{
    ui.time_lbl = label(tile, &ilf_font_digits_120, C_WOOL, "00:00");
    lv_obj_align(ui.time_lbl, LV_ALIGN_TOP_MID, 0, 88);

    ui.date_lbl = label(tile, &ilf_font_32, C_MUTED, "");
    lv_obj_align(ui.date_lbl, LV_ALIGN_TOP_MID, 0, 236);

    ui.year_bar = lv_bar_create(tile);
    lv_obj_remove_flag(ui.year_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ui.year_bar, 280, 14);
    lv_obj_align(ui.year_bar, LV_ALIGN_TOP_MID, 0, 316);
    lv_bar_set_range(ui.year_bar, 0, 1000);
    lv_obj_set_style_bg_color(ui.year_bar, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.year_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.year_bar, lv_color_hex(C_GOLD), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.year_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.year_bar, 7, LV_PART_INDICATOR);

    ui.year_lbl = label(tile, &ilf_font_24, C_WOOL, "");
    lv_obj_align(ui.year_lbl, LV_ALIGN_TOP_MID, 0, 344);
}

void ilf_ui_update_clock(const struct tm *t)
{
    if (!ui.time_lbl || !t) return;
    lv_label_set_text_fmt(ui.time_lbl, "%02d:%02d", t->tm_hour, t->tm_min);
    char date[64];
    ilf_format_date_ru(t, date, sizeof date);
    lv_label_set_text(ui.date_lbl, date);
    double p = ilf_year_progress_tm(t);
    lv_bar_set_value(ui.year_bar, (int32_t)(p * 10.0), LV_ANIM_OFF);
    lv_label_set_text_fmt(ui.year_lbl, "%d год прошёл на %d%%", t->tm_year + 1900, (int)p);
}

void ilf_ui_update_battery(uint8_t percent, bool charging)
{
    if (!ui.battery_lbl) return;
    lv_label_set_text_fmt(ui.battery_lbl, "%s%u%%", charging ? "+" : "", (unsigned)percent);
    lv_obj_set_style_text_color(ui.battery_lbl,
                                lv_color_hex(percent <= ILF_SOUL_HUNGRY_PERCENT && !charging ? C_NOSE : C_MUTED), 0);
}

/* ---------- навигация ---------- */

static void update_dots(void)
{
    ilf_app_t cur = ilf_ui_current_app();
    for (int i = 0; i < ILF_APP_COUNT; i++) {
        bool on = (int)cur == i;
        lv_obj_set_size(ui.dots[i], on ? 22 : 8, 8);
        lv_obj_set_style_bg_color(ui.dots[i], lv_color_hex(on ? C_WOOL : C_MUTED), 0);
    }
}

static void tile_changed(lv_event_t *e)
{
    (void)e;
    update_dots();
}

ilf_app_t ilf_ui_current_app(void)
{
    lv_obj_t *active = lv_tileview_get_tile_active(ui.tileview);
    for (int i = 0; i < ILF_APP_COUNT; i++) {
        if (ui.tiles[i] == active) return (ilf_app_t)i;
    }
    return ILF_APP_PET;
}

void ilf_ui_show_app(ilf_app_t app, bool animated)
{
    if ((unsigned)app >= ILF_APP_COUNT) return;
    lv_tileview_set_tile_by_index(ui.tileview, (uint32_t)app, 0, animated ? LV_ANIM_ON : LV_ANIM_OFF);
    update_dots();
}

void ilf_ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui.tileview = lv_tileview_create(scr);
    lv_obj_set_size(ui.tileview, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_set_style_bg_color(ui.tileview, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui.tileview, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(ui.tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(ui.tileview, tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < ILF_APP_COUNT; i++) {
        lv_dir_t dir = LV_DIR_NONE;
        if (i > 0) dir |= LV_DIR_LEFT;
        if (i < ILF_APP_COUNT - 1) dir |= LV_DIR_RIGHT;
        ui.tiles[i] = lv_tileview_add_tile(ui.tileview, (uint8_t)i, 0, dir);
    }
    build_pet(ui.tiles[ILF_APP_PET]);
    build_voice(ui.tiles[ILF_APP_VOICE]);
    build_draw(ui.tiles[ILF_APP_DRAW]);
    build_music(ui.tiles[ILF_APP_MUSIC]);
    build_clock(ui.tiles[ILF_APP_CLOCK]);

    /* поверх всех экранов: заряд сверху и точки-страницы снизу */
    lv_obj_t *top = lv_layer_top();
    ui.battery_lbl = label(top, &ilf_font_24, C_MUTED, "");
    lv_obj_align(ui.battery_lbl, LV_ALIGN_TOP_RIGHT, -28, 16);

    lv_obj_t *row = plain(top);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -14);
    for (int i = 0; i < ILF_APP_COUNT; i++) {
        ui.dots[i] = plain(row);
        lv_obj_set_style_radius(ui.dots[i], 4, 0);
        lv_obj_set_style_bg_opa(ui.dots[i], LV_OPA_COVER, 0);
    }
    ilf_ui_show_app(ILF_APP_PET, false);
}
