#include "ilf_ui.h"

#include <stdio.h>

#include "ilf_clock.h"

LV_FONT_DECLARE(ilf_font_24);
LV_FONT_DECLARE(ilf_font_32);
LV_FONT_DECLARE(ilf_font_48);
LV_FONT_DECLARE(ilf_font_digits_120);
LV_IMAGE_DECLARE(ilf_eye_big);
LV_IMAGE_DECLARE(ilf_eye_small);

/* Палитра персонажа ILF: кремовая головка на AMOLED-чёрном */
#define C_BG       0x000000
#define C_WOOL     0xF3EDE2   /* текст и светлые элементы */
#define C_HEAD     0xEEE6D9   /* мордочка и шерсть */
#define C_SHADE    0xDCD0BC   /* тени: линия шерсти, складки */
#define C_EAR_IN   0xCDBBA2   /* внутренняя сторона уха */
#define C_EYE      0x000000   /* глаза — чёрные овалы */
#define C_MOUTH    0x93795A   /* носик-«Y» и рот */
#define C_PUPIL    0x121016
#define C_NOSE     0xF2A7B5
#define C_GOLD     0xF2B84B
#define C_MUTED    0x8F879B
#define C_CARD     0x1E1A24
#define C_TEAR     0x7EC8F2

/* Геометрия по референсу assets/character/ilf_sheep_reference.png (масштаб 0.38) */
#define HEAD_CX 184
#define HEAD_CY 214
#define HEAD_W  238
#define HEAD_H  206
#define EYE_W   42
#define EYE_H   84
#define EYE_L_CX 112
#define EYE_R_CX 256
#define EYE_CY  222
#define NOSE_Y  246

typedef struct {
    lv_obj_t *eye;     /* чёрный овал */
    lv_obj_t *lid;     /* верхнее веко цвета мордочки */
    lv_obj_t *cheek;   /* нижнее веко: глаза «улыбаются» */
    lv_obj_t *closed;  /* закрытый глаз: дужка */
    int cx;
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

static lv_obj_t *arc_line(lv_obj_t *parent, int cx, int cy, int size, int start, int end, uint32_t color, int width)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_size(a, size, size);
    lv_obj_set_pos(a, cx - size / 2, cy - size / 2);
    lv_arc_set_bg_angles(a, start, end);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
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

static lv_obj_t *lid_rect(lv_obj_t *parent)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_HEAD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_size(r, EYE_W, 0);
    return r;
}

static void make_eye(eye_t *eye, lv_obj_t *parent, int cx)
{
    eye->cx = cx;
    /* глаз — сглаженный эллипс-картинка; веки — прямоугольники цвета мордочки поверх:
       за краем глаза они сливаются с мордочкой, поэтому их не видно */
    eye->eye = lv_image_create(parent);
    lv_image_set_src(eye->eye, &ilf_eye_big);
    lv_obj_remove_flag(eye->eye, LV_OBJ_FLAG_CLICKABLE);
    eye->lid = lid_rect(parent);
    eye->cheek = lid_rect(parent);
    eye->closed = arc_line(parent, cx, EYE_CY - 8, 44, 25, 155, C_PUPIL, 6);
    lv_obj_add_flag(eye->closed, LV_OBJ_FLAG_HIDDEN);
}

/* Висячее ухо: кремовый овал с бежевой серединкой, повёрнутый вниз-наружу. */
static void make_ear(lv_obj_t *parent, int cx, int cy, int angle_deci)
{
    lv_obj_t *ear = blob(parent, cx, cy, 92, 46, C_HEAD);
    lv_obj_t *in = blob(ear, 50, 23, 64, 26, C_EAR_IN);
    (void)in;
    lv_obj_set_style_transform_pivot_x(ear, 46, 0);
    lv_obj_set_style_transform_pivot_y(ear, 23, 0);
    lv_obj_set_style_transform_rotation(ear, angle_deci, 0);
}

static lv_obj_t *nose_line(lv_obj_t *parent, const lv_point_precise_t *pts, uint32_t n)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, n);
    lv_obj_set_style_line_color(l, lv_color_hex(C_MOUTH), 0);
    lv_obj_set_style_line_width(l, 3, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_pos(l, HEAD_CX - 8, NOSE_Y);
    return l;
}

static void build_pet(lv_obj_t *tile)
{
    ui.stage = plain(tile);
    lv_obj_set_size(ui.stage, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_add_flag(ui.stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.stage, pet_clicked, LV_EVENT_CLICKED, NULL);

    /* уши — под головой, висят вниз-наружу */
    make_ear(ui.stage, 50, 210, 3150);  /* 315° = наклон влево-вниз */
    make_ear(ui.stage, 318, 210, 450);

    /* кудрявая макушка: бугорки по верхнему краю */
    static const int16_t curls[][3] = {
        {96, 160, 34}, {128, 128, 36}, {168, 114, 38}, {208, 114, 38}, {246, 126, 36}, {276, 156, 34},
    };
    for (size_t i = 0; i < sizeof curls / sizeof curls[0]; i++) {
        blob(ui.stage, curls[i][0], curls[i][1], curls[i][2] * 2, curls[i][2] * 2, C_HEAD);
    }
    /* голова: широкая снизу, как у референса */
    blob(ui.stage, HEAD_CX, HEAD_CY, HEAD_W, HEAD_H, C_HEAD);
    blob(ui.stage, HEAD_CX, HEAD_CY + 34, HEAD_W - 8, HEAD_H - 70, C_HEAD);

    /* шерсть: волнистая линия чёлки и пара завитков на макушке */
    static const int16_t fringe[][2] = {
        {122, 152}, {160, 146}, {198, 145}, {236, 148}, {272, 156},
    };
    for (size_t i = 0; i < sizeof fringe / sizeof fringe[0]; i++) {
        arc_line(ui.stage, fringe[i][0], fringe[i][1], 44, 20, 160, C_SHADE, 3);
    }

    make_eye(&ui.eyes[0], ui.stage, EYE_L_CX);
    make_eye(&ui.eyes[1], ui.stage, EYE_R_CX);

    /* носик-«Y» */
    static const lv_point_precise_t y_left[]  = {{1, 1}, {8, 7}};
    static const lv_point_precise_t y_right[] = {{15, 1}, {8, 7}};
    static const lv_point_precise_t y_stem[]  = {{8, 7}, {8, 15}};
    nose_line(ui.stage, y_left, 2);
    nose_line(ui.stage, y_right, 2);
    nose_line(ui.stage, y_stem, 2);

    /* рты под носиком: появляются по эмоции */
    ui.smile = arc_line(ui.stage, HEAD_CX, NOSE_Y + 10, 28, 30, 150, C_MOUTH, 3);
    ui.frown = arc_line(ui.stage, HEAD_CX, NOSE_Y + 34, 28, 210, 330, C_MOUTH, 3);
    ui.mouth_o = blob(ui.stage, HEAD_CX, NOSE_Y + 26, 12, 14, C_MOUTH);

    ui.tear = blob(ui.stage, EYE_L_CX + 4, EYE_CY + 54, 10, 16, C_TEAR);
    ui.sweat = blob(ui.stage, 282, 150, 12, 18, C_TEAR);
    ui.zzz = label(ui.stage, &ilf_font_32, C_GOLD, "z z Z");
    lv_obj_set_pos(ui.zzz, 256, 70);

    ui.status = label(ui.stage, &ilf_font_24, C_MUTED, "");
    lv_obj_set_width(ui.status, ILF_SCREEN_W);
    lv_obj_set_style_text_align(ui.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui.status, 0, 372);

    ui.last_emotion = ILF_EMO_COUNT;
}

void ilf_ui_update_face(const ilf_face_t *f)
{
    if (!ui.stage || !f) return;

    /* у персонажа нет зрачков: взгляд — это лёгкий сдвиг самих глаз */
    int dx = (int)(f->gaze_x * 6.0f);
    int dy = (int)(f->gaze_y * 5.0f);
    bool happy = f->emotion == ILF_EMO_HAPPY;
    bool scared = f->emotion == ILF_EMO_SCARED;
    int h = scared ? EYE_H - 14 : EYE_H; /* испуганный — глаза-точки поменьше */
    int w = scared ? EYE_W - 8 : EYE_W;

    for (int i = 0; i < 2; i++) {
        eye_t *e = &ui.eyes[i];
        lv_image_set_src(e->eye, scared ? &ilf_eye_small : &ilf_eye_big);
        int ex = e->cx - w / 2 + dx, ey = EYE_CY - h / 2 + dy;
        lv_obj_set_pos(e->eye, ex, ey);
        int lid = (int)(f->eyelid * h + 0.5f);
        lv_obj_set_size(e->lid, w + 4, lid);
        lv_obj_set_pos(e->lid, ex - 2, ey - 1);
        int cheek = happy ? h * 32 / 100 : 0; /* радость: снизу поджимаются «щёчки» */
        lv_obj_set_size(e->cheek, w + 4, cheek);
        lv_obj_set_pos(e->cheek, ex - 2, ey + h - cheek + 1);
        bool closed = f->eyelid > 0.92f;
        show(e->eye, !closed);
        show(e->lid, !closed);
        show(e->cheek, !closed);
        show(e->closed, closed);
        lv_obj_set_pos(e->closed, e->cx - 22 + dx, EYE_CY - 30 + dy);
    }

    show(ui.smile, !f->mouth_open && f->mouth > 0.5f);
    show(ui.frown, !f->mouth_open && f->mouth < -0.5f);
    show(ui.mouth_o, f->mouth_open);

    if (f->emotion != ui.last_emotion) {
        ui.last_emotion = f->emotion;
        show(ui.zzz, f->emotion == ILF_EMO_ASLEEP || f->emotion == ILF_EMO_SLEEPY);
        show(ui.tear, f->emotion == ILF_EMO_SAD);
        show(ui.sweat, scared);
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
