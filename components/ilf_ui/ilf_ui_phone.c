/*
 * iPhone на экране барашка: пульт музыки, уведомления сверху, код подключения.
 * Телефон сам ничего не рисует — своего приложения нет, всё по Bluetooth.
 */
#include "ilf_ui_priv.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ilf_font_24);
LV_FONT_DECLARE(ilf_font_32);
LV_FONT_DECLARE(ilf_font_48);

#define C_BG    0x000000
#define C_WOOL  0xF3EDE2
#define C_SOFT  0xE4DED4
#define C_CARD  0x1E1C1A
#define C_NOSE  0xF2A7B5
#define C_MUTED 0x8F879B

#define NOTE_SHOW_MS 6000
#define CALL_SHOW_MS 45000

static struct {
    ilf_ui_phone_cb_t cb;
    bool connected;
    char ble_name[32];
    /* музыка */
    lv_obj_t *song, *artist, *hint, *btn[3], *play_lbl;
    bool playing;
    /* код */
    lv_obj_t *pass;
    /* уведомление */
    lv_obj_t *card;
    lv_timer_t *card_timer;
    uint32_t card_uid;
    bool card_call;
} ph;

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *mk_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *round_btn(lv_obj_t *parent, int cx, int cy, int size, uint32_t ring, int width)
{
    lv_obj_t *b = mk_box(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_pos(b, cx - size / 2, cy - size / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3836), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(ring), 0);
    lv_obj_set_style_border_width(b, width, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 10);
    return b;
}

void ilf_ui_set_phone_cb(const ilf_ui_phone_cb_t *cb) { ph.cb = *cb; }

/* ---------- музыка ---------- */

static void music_clicked(lv_event_t *e)
{
    ilf_ui_music_btn_t b = (ilf_ui_music_btn_t)(intptr_t)lv_event_get_user_data(e);
    if (!ph.connected) return;
    if (b == ILF_UI_MUSIC_TOGGLE) {
        /* сразу перерисовать, не ждать телефон — так отзывчивее */
        ph.playing = !ph.playing;
        lv_label_set_text(ph.play_lbl, ph.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (ph.cb.music) ph.cb.music(b, ph.cb.user);
}

static void music_hint(void)
{
    if (!ph.hint) return;
    if (ph.connected) {
        lv_label_set_text(ph.hint, "");
    } else {
        char buf[96];
        snprintf(buf, sizeof buf, "Подключи iPhone:\nBluetooth » %s", ph.ble_name[0] ? ph.ble_name : "ILF");
        lv_label_set_text(ph.hint, buf);
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_border_color(ph.btn[i], lv_color_hex(ph.connected ? C_WOOL : C_MUTED), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(ph.btn[i], 0), lv_color_hex(ph.connected ? C_WOOL : C_MUTED), 0);
    }
}

void ilf_ui_build_music(lv_obj_t *tile)
{
    lv_obj_t *t = mk_label(tile, &ilf_font_32, C_WOOL, "Музыка");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);

    /* название и исполнитель — столбиком по центру между заголовком и кнопками */
    lv_obj_t *info = mk_box(tile);
    lv_obj_set_size(info, 330, 140);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(info, 6, 0);

    ph.song = mk_label(info, &ilf_font_32, C_WOOL, "");
    lv_obj_set_size(ph.song, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(ph.song, 84, 0);   /* не больше двух строк */
    lv_obj_set_style_text_align(ph.song, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ph.song, LV_LABEL_LONG_DOT);

    ph.artist = mk_label(info, &ilf_font_24, C_SOFT, "");
    lv_obj_set_width(ph.artist, LV_PCT(100));
    lv_obj_set_style_text_align(ph.artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ph.artist, LV_LABEL_LONG_DOT);

    static const char *syms[3] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    for (int i = 0; i < 3; i++) {
        int size = i == 1 ? 96 : 68;
        ph.btn[i] = round_btn(tile, 84 + i * 100, 262, size, C_WOOL, i == 1 ? 5 : 4);
        lv_obj_add_event_cb(ph.btn[i], music_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = mk_label(ph.btn[i], i == 1 ? &ilf_font_32 : &ilf_font_24, C_WOOL, syms[i]);
        lv_obj_center(l);
        if (i == 1) ph.play_lbl = l;
    }

    ph.hint = mk_label(tile, &ilf_font_24, C_SOFT, "");
    lv_obj_set_width(ph.hint, 340);
    lv_obj_set_style_text_align(ph.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ph.hint, LV_ALIGN_TOP_MID, 0, 330);
    ilf_ui_music("", "", false);
    music_hint();
}

void ilf_ui_phone(bool connected, const char *ble_name)
{
    ph.connected = connected;
    if (ble_name) snprintf(ph.ble_name, sizeof ph.ble_name, "%s", ble_name);
    if (!connected) ilf_ui_music("", "", false);
    music_hint();
}

void ilf_ui_music(const char *title, const char *artist, bool playing)
{
    if (!ph.song) return;
    bool empty = !title || !*title;
    lv_label_set_text(ph.song, empty ? (ph.connected ? "Ничего не играет" : "iPhone не подключён") : title);
    lv_obj_set_style_text_color(ph.song, lv_color_hex(empty ? C_MUTED : C_WOOL), 0);
    lv_label_set_text(ph.artist, artist ? artist : "");
    ph.playing = playing;
    lv_label_set_text(ph.play_lbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

/* ---------- код подключения ---------- */

void ilf_ui_passkey(int32_t code)
{
    if (ph.pass) { lv_obj_delete(ph.pass); ph.pass = NULL; }
    if (code < 0) return;
    ph.pass = mk_box(lv_layer_top());
    lv_obj_set_size(ph.pass, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_set_style_bg_color(ph.pass, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ph.pass, LV_OPA_COVER, 0);
    lv_obj_add_flag(ph.pass, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = mk_label(ph.pass, &ilf_font_32, C_WOOL, LV_SYMBOL_BLUETOOTH " iPhone");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 96);
    char buf[16];
    snprintf(buf, sizeof buf, "%03d %03d", (int)(code / 1000), (int)(code % 1000));
    lv_obj_t *d = mk_label(ph.pass, &ilf_font_48, C_WOOL, buf);
    lv_obj_set_style_text_letter_space(d, 4, 0);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_t *h = mk_label(ph.pass, &ilf_font_24, C_SOFT, "Введи этот код\nна телефоне");
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 260);
}

/* ---------- уведомление сверху ---------- */

static void card_close(void)
{
    if (ph.card_timer) { lv_timer_delete(ph.card_timer); ph.card_timer = NULL; }
    if (ph.card) { lv_obj_delete(ph.card); ph.card = NULL; }
    ph.card_call = false;
}

static void card_timeout(lv_timer_t *t)
{
    (void)t;
    ph.card_timer = NULL;   /* таймер одноразовый, LVGL удалит его сам */
    if (ph.card) { lv_obj_delete(ph.card); ph.card = NULL; }
    ph.card_call = false;
}

static void card_clicked(lv_event_t *e)
{
    (void)e;
    if (!ph.card_call) card_close();
}

static void call_clicked(lv_event_t *e)
{
    bool accept = lv_event_get_user_data(e) != NULL;
    uint32_t uid = ph.card_uid;
    card_close();
    if (ph.cb.call) ph.cb.call(uid, accept, ph.cb.user);
}

void ilf_ui_notify(const char *app, const char *title, const char *text, bool call, uint32_t uid)
{
    /* звонок не перебиваем обычным уведомлением */
    if (ph.card_call && !call) return;
    card_close();
    ph.card_uid = uid;
    ph.card_call = call;

    lv_obj_t *c = ph.card = mk_box(lv_layer_top());
    lv_obj_set_width(c, ILF_SCREEN_W - 24);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_radius(c, 28, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x3A3836), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, card_clicked, LV_EVENT_CLICKED, NULL);

    char head[64];
    snprintf(head, sizeof head, LV_SYMBOL_BELL "  %s", call ? "Звонок" : (app ? app : ""));
    lv_obj_t *a = mk_label(c, &ilf_font_24, C_SOFT, head);
    lv_obj_set_width(a, LV_PCT(100));
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);

    if (title && *title) {
        lv_obj_t *t = mk_label(c, &ilf_font_32, C_WOOL, title);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    }
    if (!call && text && *text) {
        lv_obj_t *m = mk_label(c, &ilf_font_24, C_SOFT, text);
        lv_obj_set_size(m, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(m, 64, 0);   /* две строки */
        lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
    }
    if (call) {
        lv_obj_t *row = mk_box(c);
        lv_obj_set_size(row, LV_PCT(100), 92);
        lv_obj_t *no = round_btn(row, 80, 46, 80, C_NOSE, 5);
        lv_obj_add_event_cb(no, call_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_center(mk_label(no, &ilf_font_48, C_NOSE, "×"));
        lv_obj_t *yes = round_btn(row, 240, 46, 80, C_WOOL, 5);
        lv_obj_add_event_cb(yes, call_clicked, LV_EVENT_CLICKED, (void *)1);
        lv_obj_center(mk_label(yes, &ilf_font_32, C_WOOL, LV_SYMBOL_OK));
    }
    ph.card_timer = lv_timer_create(card_timeout, call ? CALL_SHOW_MS : NOTE_SHOW_MS, NULL);
    lv_timer_set_repeat_count(ph.card_timer, 1);
}

void ilf_ui_notify_gone(uint32_t uid)
{
    if (ph.card && ph.card_uid == uid) card_close();
}
