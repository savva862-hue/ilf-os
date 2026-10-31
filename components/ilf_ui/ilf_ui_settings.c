/*
 * Экран «Настройки»: громкость динамика, домашний Wi-Fi, Яндекс Диск.
 *
 * Wi-Fi нужен только для выгрузки голосовых и рисунков на Диск: барашек
 * подключается ненадолго, отдаёт файлы и возвращается к стаду.
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
#define C_MUTED 0x8F879B
#define C_KEY   0x262422
#define C_KEY_P 0x4A4642
#define C_TRACK 0x3A3836

#define WIFI_ROWS 8

static struct {
    ilf_ui_settings_cb_t cb;
    lv_obj_t *vol_bar, *vol_lbl, *wifi_lbl, *disk_lbl, *ver_lbl;
    int volume;
    /* окно поверх всего: список сетей, пароль или код Диска */
    lv_obj_t *over, *list, *list_msg, *ta, *kbd;
    char ssids[WIFI_ROWS][33];
    int n_ssids;
    char join_ssid[33];
} s;

/* ---------- помощники ---------- */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *str)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, str);
    return l;
}

static lv_obj_t *key(lv_obj_t *parent, int w, int h, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h >= 60 ? 26 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 6);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

/* ---------- громкость ---------- */

static void vol_show(void)
{
    if (!s.vol_bar) return;
    lv_bar_set_value(s.vol_bar, s.volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(s.vol_lbl, "Громкость %d%%", s.volume);
}

static void vol_clicked(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int v = s.volume + step;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == s.volume) return;
    s.volume = v;
    vol_show();
    if (s.cb.volume) s.cb.volume(v, s.cb.user);
}

void ilf_ui_settings_volume(int percent)
{
    s.volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    vol_show();
}

/* ---------- окно поверх ---------- */

static void over_close(void)
{
    if (s.over) lv_obj_delete(s.over);
    s.over = s.list = s.list_msg = s.ta = s.kbd = NULL;
}

static void over_close_clicked(lv_event_t *e)
{
    (void)e;
    over_close();
}

static lv_obj_t *over_open(const char *title)
{
    over_close();
    lv_obj_t *o = s.over = box(lv_layer_top());
    lv_obj_set_size(o, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *x = key(o, 52, 52, over_close_clicked, NULL);
    lv_obj_set_pos(x, 14, 8);
    lv_obj_center(text(x, &ilf_font_32, C_WOOL, "×"));
    lv_obj_t *t = text(o, &ilf_font_32, C_WOOL, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
    return o;
}

/* ---------- Wi-Fi ---------- */

static void wifi_join_ready(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) { over_close(); return; }
    if (code != LV_EVENT_READY) return;
    const char *pass = lv_textarea_get_text(s.ta);
    if (s.cb.wifi_join) s.cb.wifi_join(s.join_ssid, pass, s.cb.user);
    over_close();
    ilf_ui_wifi_status("подключаюсь…");
}

/* Свои раскладки: в шрифте барашка нет значков стандартной клавиатуры LVGL */
#define K(w) ((lv_buttonmatrix_ctrl_t)(w))
static const char *const kb_lower[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "-", "z", "x", "c", "v", "b", "n", "m", ".", LV_SYMBOL_BACKSPACE, "\n",
    " ", LV_SYMBOL_OK, "" };
static const char *const kb_upper[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "-", "Z", "X", "C", "V", "B", "N", "M", ".", LV_SYMBOL_BACKSPACE, "\n",
    " ", LV_SYMBOL_OK, "" };
static const char *const kb_special[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "abc", "!", "@", "#", "$", "%", "&", "*", "(", ")", "\n",
    "_", "+", "=", "/", "?", ":", ";", "'", ",", LV_SYMBOL_BACKSPACE, "\n",
    " ", LV_SYMBOL_OK, "" };
static const lv_buttonmatrix_ctrl_t kb_ctrl_letters[] = {
    K(4), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3),
    K(5), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4),
    K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(5),
    K(6), K(2) };
static const lv_buttonmatrix_ctrl_t kb_ctrl_special[] = {
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    K(5), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4),
    K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(5),
    K(6), K(2) };

static void wifi_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s.n_ssids) return;
    snprintf(s.join_ssid, sizeof s.join_ssid, "%s", s.ssids[i]);
    lv_obj_t *o = over_open(s.join_ssid);

    s.ta = lv_textarea_create(o);
    lv_textarea_set_one_line(s.ta, true);
    lv_textarea_set_placeholder_text(s.ta, "пароль");
    lv_textarea_set_max_length(s.ta, 63);
    lv_obj_set_size(s.ta, 330, 56);
    lv_obj_align(s.ta, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_text_font(s.ta, &ilf_font_24, 0);
    lv_obj_set_style_text_color(s.ta, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_bg_color(s.ta, lv_color_hex(C_KEY), 0);
    lv_obj_set_style_border_width(s.ta, 0, 0);
    lv_obj_set_style_radius(s.ta, 18, 0);
    lv_obj_set_style_text_color(s.ta, lv_color_hex(C_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s.ta, lv_color_hex(C_WOOL), LV_PART_CURSOR);
    lv_obj_add_state(s.ta, LV_STATE_FOCUSED);

    /* полная латинская клавиатура: пароли бывают с цифрами и знаками */
    s.kbd = lv_keyboard_create(o);
    lv_obj_set_size(s.kbd, ILF_SCREEN_W, 300);
    lv_obj_align(s.kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s.kbd, s.ta);
    lv_keyboard_set_map(s.kbd, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lower, kb_ctrl_letters);
    lv_keyboard_set_map(s.kbd, LV_KEYBOARD_MODE_TEXT_UPPER, kb_upper, kb_ctrl_letters);
    lv_keyboard_set_map(s.kbd, LV_KEYBOARD_MODE_SPECIAL, kb_special, kb_ctrl_special);
    lv_keyboard_set_mode(s.kbd, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(s.kbd, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(s.kbd, 4, 0);
    lv_obj_set_style_pad_gap(s.kbd, 4, 0);
    lv_obj_set_style_bg_color(s.kbd, lv_color_hex(C_KEY), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s.kbd, lv_color_hex(C_KEY_P), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s.kbd, lv_color_hex(C_KEY_P), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s.kbd, lv_color_hex(C_WOOL), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s.kbd, &ilf_font_24, LV_PART_ITEMS);
    lv_obj_set_style_radius(s.kbd, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s.kbd, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s.kbd, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s.kbd, wifi_join_ready, LV_EVENT_ALL, NULL);
}

static void wifi_rows(void)
{
    if (!s.list) return;
    lv_obj_clean(s.list);
    for (int i = 0; i < s.n_ssids; i++) {
        lv_obj_t *b = key(s.list, 320, 60, wifi_pick, (void *)(intptr_t)i);
        lv_obj_t *l = text(b, &ilf_font_24, C_WOOL, s.ssids[i]);
        lv_obj_set_width(l, 290);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_center(l);
    }
    lv_label_set_text(s.list_msg, s.n_ssids ? "" : "Ищу сети…");
}

static void wifi_forget_clicked(lv_event_t *e)
{
    (void)e;
    if (s.cb.wifi_forget) s.cb.wifi_forget(s.cb.user);
    over_close();
}

static void wifi_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_t *o = over_open("Wi-Fi");
    s.list = box(o);
    lv_obj_set_size(s.list, ILF_SCREEN_W, 290);
    lv_obj_set_pos(s.list, 0, 72);
    lv_obj_add_flag(s.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s.list, 10, 0);
    s.list_msg = text(o, &ilf_font_24, C_MUTED, "");
    lv_obj_align(s.list_msg, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_t *f = key(o, 220, 52, wifi_forget_clicked, NULL);
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_center(text(f, &ilf_font_24, C_SOFT, "Забыть сеть"));
    s.n_ssids = 0;
    wifi_rows();
    if (s.cb.wifi_scan) s.cb.wifi_scan(s.cb.user);
}

void ilf_ui_wifi_list(const char *const *ssids, int n)
{
    s.n_ssids = 0;
    for (int i = 0; i < n && s.n_ssids < WIFI_ROWS; i++) {
        if (!ssids[i] || !ssids[i][0]) continue;
        snprintf(s.ssids[s.n_ssids++], sizeof s.ssids[0], "%s", ssids[i]);
    }
    wifi_rows();
    if (s.list && !s.n_ssids) lv_label_set_text(s.list_msg, "Сетей не нашлось");
}

void ilf_ui_wifi_status(const char *str)
{
    if (s.wifi_lbl) lv_label_set_text(s.wifi_lbl, str && *str ? str : "не задан");
}

/* ---------- Яндекс Диск ---------- */

static void disk_clicked(lv_event_t *e)
{
    (void)e;
    if (s.cb.disk_link) s.cb.disk_link(s.cb.user);
}

void ilf_ui_disk_status(const char *str)
{
    if (s.disk_lbl) lv_label_set_text(s.disk_lbl, str && *str ? str : "не привязан");
}

void ilf_ui_disk_code(const char *url, const char *code)
{
    if (!url || !code) { if (s.over && !s.list && !s.kbd) over_close(); return; }
    lv_obj_t *o = over_open("Яндекс Диск");
    lv_obj_t *a = text(o, &ilf_font_24, C_SOFT, "Открой на телефоне");
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_t *u = text(o, &ilf_font_32, C_WOOL, url);
    lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_t *b = text(o, &ilf_font_24, C_SOFT, "и введи код");
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_t *c = text(o, &ilf_font_48, C_WOOL, code);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_t *w = text(o, &ilf_font_24, C_MUTED, "жду подтверждения…");
    lv_obj_align(w, LV_ALIGN_TOP_MID, 0, 320);
}

/* ---------- плитка ---------- */

static lv_obj_t *row(lv_obj_t *tile, int y, const char *name, lv_obj_t **value, lv_event_cb_t cb)
{
    lv_obj_t *b = key(tile, 320, 76, cb, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t *n = text(b, &ilf_font_24, C_WOOL, name);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 22, 8);
    *value = text(b, &ilf_font_24, C_MUTED, "");
    lv_obj_set_width(*value, 280);
    lv_label_set_long_mode(*value, LV_LABEL_LONG_DOT);
    lv_obj_align(*value, LV_ALIGN_TOP_LEFT, 22, 40);
    return b;
}

void ilf_ui_build_settings(lv_obj_t *tile)
{
    lv_obj_t *t = text(tile, &ilf_font_32, C_WOOL, "Настройки");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);

    /* громкость: «−» полоска «+». Ползунок не годится — свайп листает экраны */
    s.vol_lbl = text(tile, &ilf_font_24, C_SOFT, "");
    lv_obj_align(s.vol_lbl, LV_ALIGN_TOP_MID, 0, 72);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = key(tile, 56, 56, vol_clicked, (void *)(intptr_t)(i ? 10 : -10));
        lv_obj_set_pos(b, i ? ILF_SCREEN_W - 24 - 56 : 24, 104);
        lv_obj_center(text(b, &ilf_font_32, C_WOOL, i ? "+" : "-"));
    }
    s.vol_bar = lv_bar_create(tile);
    lv_obj_remove_flag(s.vol_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s.vol_bar, 176, 14);
    lv_obj_align(s.vol_bar, LV_ALIGN_TOP_MID, 0, 125);
    lv_bar_set_range(s.vol_bar, 0, 100);
    lv_obj_set_style_bg_color(s.vol_bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.vol_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.vol_bar, lv_color_hex(C_WOOL), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s.vol_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s.vol_bar, 7, LV_PART_INDICATOR);
    if (!s.volume) s.volume = 70;
    vol_show();

    row(tile, 182, "Wi-Fi", &s.wifi_lbl, wifi_clicked);
    row(tile, 272, "Яндекс Диск", &s.disk_lbl, disk_clicked);
    ilf_ui_wifi_status(NULL);
    ilf_ui_disk_status(NULL);

    s.ver_lbl = text(tile, &ilf_font_24, C_MUTED, "");
    lv_obj_align(s.ver_lbl, LV_ALIGN_TOP_MID, 0, 376);
}

void ilf_ui_set_settings_cb(const ilf_ui_settings_cb_t *cb) { s.cb = *cb; }

void ilf_ui_set_version(int version)
{
    if (s.ver_lbl) lv_label_set_text_fmt(s.ver_lbl, "ILF OS v%d", version);
}
