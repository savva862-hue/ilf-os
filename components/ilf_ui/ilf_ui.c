#include "ilf_ui.h"

#include <stdio.h>
#include <string.h>

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
#define C_EYE      0x000000   /* глаза — чёрные овалы */
#define C_MOUTH    0x93795A   /* носик-«Y» и рот */
#define C_PUPIL    0x121016
#define C_NOSE     0xF2A7B5
#define C_MUTED    0x8F879B
#define C_SOFT     0xE4DED4   /* подписи: макет v2 весь бело-кремовый */
#define C_TRACK    0x3A3836

/* Голова — картинка из макета assets/ui/ilf_ui_mockup_v2.png (tools/gen_head.py),
   глаза, веки и рты рисуются поверх неё кодом, чтобы барашек оставался живым */
#include "ilf_head_geom.h"
LV_IMAGE_DECLARE(ilf_head);

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
    /* стадо */
    ilf_ui_herd_cb_t herd;
    lv_obj_t *pet_note;
    lv_obj_t *targets[2];      /* «Кому» на голосовых и на рисунках */
    lv_obj_t *rec_btn, *rec_dot, *voice_lbl;
    lv_obj_t *rows[ILF_UI_VOICE_ROWS], *row_lbl[ILF_UI_VOICE_ROWS], *row_dot[ILF_UI_VOICE_ROWS];
    lv_obj_t *canvas;
    lv_point_t last_pt;
    uint8_t stroke_id;
    ilf_stroke_t pending;
    uint32_t pending_ms;
    struct { uint8_t id; bool has; lv_point_t pt; } remote[ILF_HERD_MAX_FRIENDS];
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
    lv_obj_t *s = label(tile, &ilf_font_24, C_SOFT, subtitle);
    lv_obj_set_width(s, 340);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s, t, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
    if (soon) {
        lv_obj_t *b = label(tile, &ilf_font_24, C_SOFT, soon);
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

/* Веко — окошко, в котором видна та же картинка головы. Закрывая глаз, оно
   показывает настоящую шерсть с тенями, поэтому краёв века не видно. */
static lv_obj_t *lid_window(lv_obj_t *parent)
{
    lv_obj_t *w = plain(parent);
    lv_obj_set_size(w, EYE_W, 0);
    lv_obj_t *img = lv_image_create(w);
    lv_image_set_src(img, &ilf_head);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return w;
}

static void place_lid(lv_obj_t *w, int x, int y, int width, int height)
{
    lv_obj_set_pos(w, x, y);
    lv_obj_set_size(w, width, height);
    lv_obj_set_pos(lv_obj_get_child(w, 0), HEAD_IMG_X - x, HEAD_IMG_Y - y);
}

static void make_eye(eye_t *eye, lv_obj_t *parent, int cx)
{
    eye->cx = cx;
    eye->eye = lv_image_create(parent);
    lv_image_set_src(eye->eye, &ilf_eye_big);
    lv_obj_remove_flag(eye->eye, LV_OBJ_FLAG_CLICKABLE);
    eye->lid = lid_window(parent);
    eye->cheek = lid_window(parent);
    eye->closed = arc_line(parent, cx, EYE_CY - 6, 40, 25, 155, C_PUPIL, 5);
    lv_obj_add_flag(eye->closed, LV_OBJ_FLAG_HIDDEN);
}

static void build_pet(lv_obj_t *tile)
{
    ui.stage = plain(tile);
    lv_obj_set_size(ui.stage, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_add_flag(ui.stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.stage, pet_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *head = lv_image_create(ui.stage);
    lv_image_set_src(head, &ilf_head);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(head, HEAD_IMG_X, HEAD_IMG_Y);

    make_eye(&ui.eyes[0], ui.stage, EYE_L_CX);
    make_eye(&ui.eyes[1], ui.stage, EYE_R_CX);

    /* рты под носиком «Y» (он сам в картинке): появляются по эмоции */
    ui.smile = arc_line(ui.stage, HEAD_CX, NOSE_Y - 2, 18, 30, 150, C_MOUTH, 3);
    ui.frown = arc_line(ui.stage, HEAD_CX, NOSE_Y + 16, 18, 210, 330, C_MOUTH, 3);
    ui.mouth_o = blob(ui.stage, HEAD_CX, NOSE_Y + 10, 9, 12, C_PUPIL);

    ui.tear = blob(ui.stage, EYE_L_CX - 10, EYE_CY + EYE_H / 2 + 6, 9, 14, C_PUPIL);
    ui.sweat = blob(ui.stage, 262, 140, 9, 14, C_PUPIL);
    ui.zzz = label(ui.stage, &ilf_font_32, C_WOOL, "z z Z");
    lv_obj_set_pos(ui.zzz, 242, 40);

    ui.status = label(ui.stage, &ilf_font_24, C_WOOL, "");
    lv_obj_set_width(ui.status, ILF_SCREEN_W);
    lv_obj_set_style_text_align(ui.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui.status, 0, 340);

    ui.pet_note = label(ui.stage, &ilf_font_24, C_SOFT, "");
    lv_obj_set_width(ui.pet_note, ILF_SCREEN_W);
    lv_obj_set_style_text_align(ui.pet_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui.pet_note, 0, 376);
    lv_label_set_long_mode(ui.pet_note, LV_LABEL_LONG_DOT);

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
    const lv_image_dsc_t *src = scared ? &ilf_eye_small : &ilf_eye_big;
    int w = src->header.w, h = src->header.h;

    for (int i = 0; i < 2; i++) {
        eye_t *e = &ui.eyes[i];
        lv_image_set_src(e->eye, src);
        int ex = e->cx - w / 2 + dx, ey = EYE_CY - h / 2 + dy;
        lv_obj_set_pos(e->eye, ex, ey);
        int lid = (int)(f->eyelid * h + 0.5f);
        place_lid(e->lid, ex - 2, ey - 2, w + 4, lid > 0 ? lid + 2 : 0);
        int cheek = happy ? h * 30 / 100 : 0; /* радость: снизу поджимаются «щёчки» */
        place_lid(e->cheek, ex - 2, ey + h - cheek, w + 4, cheek > 0 ? cheek + 2 : 0);
        bool closed = f->eyelid > 0.92f;
        show(e->eye, !closed);
        show(e->lid, !closed);
        show(e->cheek, !closed);
        show(e->closed, closed);
        if (closed) {
            /* закрытые глаза: шерсть на месте глаза и дужка поверх */
            place_lid(e->lid, ex - 2, ey - 2, w + 4, h + 4);
            show(e->lid, true);
            lv_obj_move_foreground(e->closed);
        }
        lv_obj_set_pos(e->closed, e->cx - 20 + dx, EYE_CY - 26 + dy);
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

/* «Кому: …» — пилюля, тап переключает адресата */
static void target_clicked(lv_event_t *e)
{
    (void)e;
    if (ui.herd.next_target) ui.herd.next_target(ui.herd.user);
}

static lv_obj_t *target_pill(lv_obj_t *tile, int y)
{
    lv_obj_t *pill = plain(tile);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(pill, 18, 0);
    lv_obj_set_style_pad_ver(pill, 6, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_border_width(pill, 3, 0);
    lv_obj_add_event_cb(pill, target_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(pill, &ilf_font_24, C_WOOL, "Кому: всё стадо");
    lv_obj_center(l);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, y);
    return pill;
}

void ilf_ui_set_target(const char *name)
{
    for (int i = 0; i < 2; i++) {
        if (!ui.targets[i]) continue;
        lv_label_set_text_fmt(lv_obj_get_child(ui.targets[i], 0), "Кому: %s", name);
    }
}

void ilf_ui_set_herd_cb(const ilf_ui_herd_cb_t *cb) { ui.herd = *cb; }

/* --- Голосовые --- */

static void rec_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!ui.herd.record) return;
    if (code == LV_EVENT_PRESSED) ui.herd.record(true, ui.herd.user);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) ui.herd.record(false, ui.herd.user);
}

static void row_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (ui.herd.play) ui.herd.play(i, ui.herd.user);
}

static void build_voice(lv_obj_t *tile)
{
    lv_obj_t *t = label(tile, &ilf_font_48, C_WOOL, "Голосовые");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 50);
    ui.targets[0] = target_pill(tile, 114);

    for (int i = 0; i < ILF_UI_VOICE_ROWS; i++) {
        lv_obj_t *r = plain(tile);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(r, 300, 34);
        lv_obj_set_pos(r, 34, 166 + i * 36);
        lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui.row_dot[i] = blob(r, 7, 17, 12, 12, C_NOSE);
        ui.row_lbl[i] = label(r, &ilf_font_24, C_SOFT, "");
        lv_obj_set_pos(ui.row_lbl[i], 24, 2);
        ui.rows[i] = r;
        show(r, false);
    }

    ui.rec_btn = blob(tile, ILF_SCREEN_W / 2, 334, 92, 92, C_BG);
    lv_obj_add_flag(ui.rec_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_color(ui.rec_btn, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_border_width(ui.rec_btn, 5, 0);
    lv_obj_add_event_cb(ui.rec_btn, rec_event, LV_EVENT_ALL, NULL);
    ui.rec_dot = blob(ui.rec_btn, 41, 41, 30, 30, C_WOOL);

    ui.voice_lbl = label(tile, &ilf_font_24, C_SOFT, "");
    lv_obj_set_width(ui.voice_lbl, ILF_SCREEN_W);
    lv_obj_set_style_text_align(ui.voice_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui.voice_lbl, 0, 392);
    ilf_ui_voice_state(ILF_VOICE_IDLE, 0);
}

void ilf_ui_voice_state(ilf_voice_state_t st, float value)
{
    if (!ui.voice_lbl) return;
    bool rec = st == ILF_VOICE_RECORDING;
    /* запись: точка становится квадратиком «стоп» и краснеет */
    lv_obj_set_style_radius(ui.rec_dot, rec ? 8 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui.rec_dot, lv_color_hex(rec ? C_NOSE : C_WOOL), 0);
    lv_obj_set_style_border_color(ui.rec_btn, lv_color_hex(rec ? C_NOSE : C_WOOL), 0);
    switch (st) {
    case ILF_VOICE_IDLE:       lv_label_set_text(ui.voice_lbl, "Зажми и говори"); break;
    case ILF_VOICE_NO_FRIENDS: lv_label_set_text(ui.voice_lbl, "Стукнись с другом, чтобы дружить"); break;
    case ILF_VOICE_RECORDING:  lv_label_set_text_fmt(ui.voice_lbl, "0:%02d · отпусти, и уйдёт", (int)value); break;
    case ILF_VOICE_SENDING:    lv_label_set_text_fmt(ui.voice_lbl, "Отправляю… %d%%", (int)(value * 100)); break;
    case ILF_VOICE_SENT:       lv_label_set_text(ui.voice_lbl, "Доставлено"); break;
    case ILF_VOICE_FAILED:     lv_label_set_text(ui.voice_lbl, "Друг не рядом, не дошло"); break;
    case ILF_VOICE_PLAYING:    lv_label_set_text(ui.voice_lbl, "Слушаем…"); break;
    }
}

void ilf_ui_voice_list(const ilf_ui_voice_item_t *items, int count)
{
    for (int i = 0; i < ILF_UI_VOICE_ROWS; i++) {
        bool on = i < count;
        show(ui.rows[i], on);
        if (!on) continue;
        lv_label_set_text_fmt(ui.row_lbl[i], "%s · 0:%02u", items[i].from, (unsigned)items[i].seconds);
        lv_obj_set_style_bg_color(ui.row_dot[i], lv_color_hex(items[i].unplayed ? C_NOSE : C_TRACK), 0);
    }
}

/* --- Рисунки --- */

#define CANVAS_W 320
#define CANVAS_H 236
#define PEN_W    7

static void canvas_line(lv_point_t a, lv_point_t b, uint32_t color)
{
    lv_layer_t layer;
    lv_canvas_init_layer(ui.canvas, &layer);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = PEN_W;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = a.x; d.p1.y = a.y;
    d.p2.x = b.x; d.p2.y = b.y;
    lv_draw_line(&layer, &d);
    lv_canvas_finish_layer(ui.canvas, &layer);
}

static void flush_stroke(bool force)
{
    ilf_stroke_t *p = &ui.pending;
    if (!p->count) return;
    if (!force && p->count < 10 && lv_tick_elaps(ui.pending_ms) < 60) return;
    if (ui.herd.stroke) ui.herd.stroke(p, ui.herd.user);
    /* следующий кусок начинается с последней точки — у друга линия не рвётся */
    p->pts[0] = p->pts[p->count - 1];
    p->count = force ? 0 : 1;
    ui.pending_ms = lv_tick_get();
}

static void add_point(lv_point_t pt)
{
    ilf_stroke_t *p = &ui.pending;
    if (p->count >= ILF_HERD_STROKE_POINTS) flush_stroke(true), p->count = 0;
    p->pts[p->count].x = (uint16_t)pt.x;
    p->pts[p->count].y = (uint16_t)pt.y;
    p->count++;
}

static void canvas_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t a;
    lv_obj_get_coords(ui.canvas, &a);
    pt.x = LV_CLAMP(0, pt.x - a.x1, CANVAS_W - 1);
    pt.y = LV_CLAMP(0, pt.y - a.y1, CANVAS_H - 1);

    if (code == LV_EVENT_PRESSED) {
        ui.stroke_id++;
        ui.pending = (ilf_stroke_t){ .stroke_id = ui.stroke_id };
        ui.pending_ms = lv_tick_get();
        canvas_line(pt, pt, C_WOOL);
        add_point(pt);
        ui.last_pt = pt;
    } else if (code == LV_EVENT_PRESSING) {
        int dx = pt.x - ui.last_pt.x, dy = pt.y - ui.last_pt.y;
        if (dx * dx + dy * dy < 9) return;
        canvas_line(ui.last_pt, pt, C_WOOL);
        add_point(pt);
        ui.last_pt = pt;
        flush_stroke(false);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        flush_stroke(true);
    }
}

void ilf_ui_draw_clear(void)
{
    if (!ui.canvas) return;
    lv_canvas_fill_bg(ui.canvas, lv_color_hex(C_BG), LV_OPA_COVER);
    memset(ui.remote, 0, sizeof ui.remote);
}

static void clear_clicked(lv_event_t *e)
{
    (void)e;
    ilf_ui_draw_clear();
    if (ui.herd.clear) ui.herd.clear(ui.herd.user);
}

void ilf_ui_draw_remote(int fi, const ilf_stroke_t *s)
{
    if (!ui.canvas || fi < 0 || fi >= ILF_HERD_MAX_FRIENDS) return;
    if (s->clear) { ilf_ui_draw_clear(); return; }
    for (int i = 0; i < s->count; i++) {
        lv_point_t pt = { LV_MIN(s->pts[i].x, CANVAS_W - 1), LV_MIN(s->pts[i].y, CANVAS_H - 1) };
        lv_point_t from = (i == 0) ? pt : (lv_point_t){ LV_MIN(s->pts[i - 1].x, CANVAS_W - 1), LV_MIN(s->pts[i - 1].y, CANVAS_H - 1) };
        canvas_line(from, pt, C_NOSE);
    }
    ui.remote[fi].id = s->stroke_id;
    ui.remote[fi].has = true;
}

static void build_draw(lv_obj_t *tile)
{
    lv_obj_t *t = label(tile, &ilf_font_32, C_WOOL, "Рисунки");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    ui.targets[1] = target_pill(tile, 68);

    lv_obj_t *frame = plain(tile);
    lv_obj_set_size(frame, CANVAS_W + 20, CANVAS_H + 20);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_radius(frame, 24, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_border_width(frame, 4, 0);

    lv_draw_buf_t *buf = lv_draw_buf_create(CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565, 0);
    ui.canvas = lv_canvas_create(frame);
    lv_canvas_set_draw_buf(ui.canvas, buf);
    lv_obj_center(ui.canvas);
    lv_obj_add_flag(ui.canvas, LV_OBJ_FLAG_CLICKABLE);
    /* палец на холсте рисует, а не листает приложения */
    lv_obj_remove_flag(ui.canvas, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(ui.canvas, canvas_event, LV_EVENT_ALL, NULL);
    ilf_ui_draw_clear();

    lv_obj_t *clr = label(tile, &ilf_font_24, C_SOFT, "Очистить");
    lv_obj_add_flag(clr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(clr, 16);
    lv_obj_align(clr, LV_ALIGN_TOP_MID, 0, 386);
    lv_obj_add_event_cb(clr, clear_clicked, LV_EVENT_CLICKED, NULL);
}

void ilf_ui_pet_note(const char *text)
{
    if (!ui.pet_note) return;
    lv_label_set_text(ui.pet_note, text ? text : "");
}

static void build_music(lv_obj_t *tile)
{
    app_title(tile, "Музыка", "Пульт того, что играет на iPhone", "скоро · v0.3");
    static const char *syms[3] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    for (int i = 0; i < 3; i++) {
        int size = i == 1 ? 90 : 60;
        lv_obj_t *b = blob(tile, 84 + i * 100, 284, size, size, C_BG);
        lv_obj_set_style_border_color(b, lv_color_hex(C_WOOL), 0);
        lv_obj_set_style_border_width(b, i == 1 ? 5 : 4, 0);
        lv_obj_t *l = label(b, i == 1 ? &lv_font_montserrat_32 : &lv_font_montserrat_14, C_WOOL, syms[i]);
        lv_obj_center(l);
    }
}

static void build_clock(lv_obj_t *tile)
{
    ui.time_lbl = label(tile, &ilf_font_digits_120, C_WOOL, "00:00");
    lv_obj_align(ui.time_lbl, LV_ALIGN_TOP_MID, 0, 88);

    ui.date_lbl = label(tile, &ilf_font_32, C_WOOL, "");
    lv_obj_align(ui.date_lbl, LV_ALIGN_TOP_MID, 0, 236);

    ui.year_bar = lv_bar_create(tile);
    lv_obj_remove_flag(ui.year_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ui.year_bar, 280, 14);
    lv_obj_align(ui.year_bar, LV_ALIGN_TOP_MID, 0, 316);
    lv_bar_set_range(ui.year_bar, 0, 1000);
    lv_obj_set_style_bg_color(ui.year_bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.year_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.year_bar, lv_color_hex(C_WOOL), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.year_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.year_bar, 7, LV_PART_INDICATOR);

    ui.year_lbl = label(tile, &ilf_font_24, C_SOFT, "");
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
                                lv_color_hex(percent <= ILF_SOUL_HUNGRY_PERCENT && !charging ? C_NOSE : C_WOOL), 0);
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
        /* сами экраны не прокручиваются, свайп достаётся листанию приложений */
        lv_obj_remove_flag(ui.tiles[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    build_pet(ui.tiles[ILF_APP_PET]);
    build_voice(ui.tiles[ILF_APP_VOICE]);
    build_draw(ui.tiles[ILF_APP_DRAW]);
    build_music(ui.tiles[ILF_APP_MUSIC]);
    build_clock(ui.tiles[ILF_APP_CLOCK]);

    /* поверх всех экранов: заряд сверху и точки-страницы снизу */
    lv_obj_t *top = lv_layer_top();
    ui.battery_lbl = label(top, &ilf_font_24, C_WOOL, "");
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
