/*
 * Первый запуск: знакомство с хозяином. PIN и замка нет — барашек для друзей.
 *
 * Экран крошечный (1.8"), поэтому клавиатура крупная, латиница:
 * 16 букв на странице (4×4) и две страницы алфавита. Первая буква имени — заглавная,
 * остальные — строчные, сами.
 */
#include "ilf_ui.h"

#include <string.h>

#include "ilf_head_geom.h"

LV_FONT_DECLARE(ilf_font_24);
LV_FONT_DECLARE(ilf_font_32);
LV_FONT_DECLARE(ilf_font_48);
LV_IMAGE_DECLARE(ilf_head);

#define C_BG    0x000000
#define C_WOOL  0xF3EDE2
#define C_SOFT  0xE4DED4
#define C_KEY   0x262422
#define C_KEY_P 0x4A4642
#define C_NOSE  0xF2A7B5
#define C_PUPIL 0x121016

#define NAME_MAX_LETTERS 11

typedef enum { ST_HELLO, ST_OWNER, ST_SHEEP, ST_DONE } step_t;

static struct {
    lv_obj_t *root;            /* полноэкранная панель поверх всего */
    lv_obj_t *title, *hint, *field, *field_lbl, *msg;
    lv_obj_t *kbd, *next_btn, *head;
    step_t step;
    int page;
    char text[ILF_HERD_NAME_LEN];
    int letters;
    ilf_setup_result_t result;
    ilf_ui_setup_cb done;
    void *done_user;
} st;

/* ---------- помощники ---------- */

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, ILF_SCREEN_W - 24);
    lv_label_set_text(l, text);
    return l;
}

static void style_matrix(lv_obj_t *m, const lv_font_t *font)
{
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 8, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_KEY), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_KEY_P), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(m, 16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, lv_color_hex(C_WOOL), LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, font, LV_PART_ITEMS);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 220, 60);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_SOFT), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &ilf_font_32, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_BG), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

/* ---------- клавиатура имён (латиница) ---------- */

static const char *const pages[2][24] = {
    { "A", "B", "C", "D", "\n", "E", "F", "G", "H", "\n", "I", "J", "K", "L", "\n", "M", "N", "O", "P", "\n", NULL },
    { "Q", "R", "S", "T", "\n", "U", "V", "W", "X", "\n", "Y", "Z", "-", "\n", "space", "\n", NULL },
};
static const char *map_cache[2][32];

static const char **kbd_map(int page)
{
    const char **m = map_cache[page];
    int n = 0;
    for (int i = 0; pages[page][i]; i++) m[n++] = pages[page][i];
    /* нижний ряд: страницы алфавита, стереть, готово */
    m[n++] = "A…"; m[n++] = "Q…"; m[n++] = LV_SYMBOL_BACKSPACE; m[n++] = LV_SYMBOL_OK;
    m[n] = "";
    return m;
}

static void kbd_apply_page(void)
{
    lv_buttonmatrix_set_map(st.kbd, kbd_map(st.page));
    uint32_t keys = st.page == 0 ? 16 : 12;
    for (int i = 0; i < 2; i++) {
        uint32_t id = keys + (uint32_t)i;
        if (i == st.page) lv_buttonmatrix_set_button_ctrl(st.kbd, id, LV_BUTTONMATRIX_CTRL_CHECKED);
        else lv_buttonmatrix_clear_button_ctrl(st.kbd, id, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

static void field_refresh(void)
{
    lv_label_set_text_fmt(st.field_lbl, "%s|", st.text);
}

static void name_backspace(void)
{
    size_t n = strlen(st.text);
    if (!n) return;
    st.text[n - 1] = 0;
    st.letters--;
}

static void go(step_t s);

static void kbd_event(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(st.kbd);
    const char *t = lv_buttonmatrix_get_button_text(st.kbd, id);
    (void)e;
    if (!t || !*t) return;
    if (!strcmp(t, "A…")) { st.page = 0; kbd_apply_page(); return; }
    if (!strcmp(t, "Q…")) { st.page = 1; kbd_apply_page(); return; }
    if (!strcmp(t, LV_SYMBOL_BACKSPACE)) { name_backspace(); field_refresh(); return; }
    if (!strcmp(t, LV_SYMBOL_OK)) {
        /* убрать пробелы по краям */
        while (st.letters && st.text[strlen(st.text) - 1] == ' ') name_backspace();
        if (!st.letters) { lv_label_set_text(st.msg, "Нужна хотя бы одна буква"); return; }
        go(st.step == ST_OWNER ? ST_SHEEP : ST_DONE);
        return;
    }
    if (st.letters >= NAME_MAX_LETTERS) return;
    char c;
    if (!strcmp(t, "space")) { if (!st.letters) return; c = ' '; }
    else {
        /* первая буква слова — заглавная, остальные — строчные */
        char prev = st.letters ? st.text[st.letters - 1] : ' ';
        c = (prev == ' ' || prev == '-') ? t[0] : (char)(t[0] >= 'A' && t[0] <= 'Z' ? t[0] + 32 : t[0]);
    }
    st.text[st.letters++] = c;
    st.text[st.letters] = 0;
    lv_label_set_text(st.msg, "");
    field_refresh();
}

/* ---------- шаги ---------- */

static void next_clicked(lv_event_t *e)
{
    (void)e;
    if (st.step == ST_HELLO) { go(ST_OWNER); return; }
    if (st.step == ST_DONE) {
        lv_obj_delete(st.root);
        st.root = NULL;
        if (st.done) st.done(&st.result, st.done_user);
    }
}

static void build_root(void)
{
    if (st.root) lv_obj_delete(st.root);
    st.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(st.root);
    lv_obj_set_size(st.root, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_set_style_bg_color(st.root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(st.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(st.root, LV_OBJ_FLAG_CLICKABLE);   /* не пропускать касания под панель */
    lv_obj_remove_flag(st.root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    st.title = st.hint = st.field = st.field_lbl = st.msg = st.kbd = st.next_btn = st.head = NULL;
}

static void build_name_step(const char *title)
{
    st.title = mk_label(st.root, &ilf_font_32, C_WOOL, title);
    lv_obj_align(st.title, LV_ALIGN_TOP_MID, 0, 22);

    st.field = lv_obj_create(st.root);
    lv_obj_remove_style_all(st.field);
    lv_obj_set_size(st.field, 320, 56);
    lv_obj_align(st.field, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(st.field, 18, 0);
    lv_obj_set_style_border_color(st.field, lv_color_hex(C_WOOL), 0);
    lv_obj_set_style_border_width(st.field, 3, 0);
    st.field_lbl = mk_label(st.field, &ilf_font_32, C_WOOL, "");
    lv_obj_center(st.field_lbl);

    st.msg = mk_label(st.root, &ilf_font_24, C_NOSE, "");
    lv_obj_align(st.msg, LV_ALIGN_TOP_MID, 0, 130);

    st.kbd = lv_buttonmatrix_create(st.root);
    style_matrix(st.kbd, &ilf_font_32);
    lv_obj_set_style_bg_color(st.kbd, lv_color_hex(C_WOOL), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(st.kbd, lv_color_hex(C_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_size(st.kbd, 352, 280);
    lv_obj_align(st.kbd, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(st.kbd, kbd_event, LV_EVENT_VALUE_CHANGED, NULL);
    st.page = 0;
    kbd_apply_page();
    field_refresh();
}

/* Голова на приветствии и в конце: барашек спит — глаза-палочки по горизонтали */
#define HELLO_SCALE 170
#define HELLO_Y     (-6)

static void sleepy_head(void)
{
    st.head = lv_image_create(st.root);
    lv_image_set_src(st.head, &ilf_head);
    lv_image_set_scale(st.head, HELLO_SCALE);
    lv_obj_align(st.head, LV_ALIGN_TOP_MID, 0, HELLO_Y);
    /* картинка масштабируется вокруг своего центра */
    int ox = (ILF_SCREEN_W - HEAD_IMG_W) / 2 + HEAD_IMG_W / 2;
    int oy = HELLO_Y + HEAD_IMG_H / 2;
    int cxs[2] = { EYE_L_CX, EYE_R_CX };
    int w = EYE_W * HELLO_SCALE / 256 + 4;
    for (int i = 0; i < 2; i++) {
        int x = ox + (cxs[i] - HEAD_IMG_X - HEAD_IMG_W / 2) * HELLO_SCALE / 256;
        int y = oy + (EYE_CY - HEAD_IMG_Y - HEAD_IMG_H / 2) * HELLO_SCALE / 256;
        lv_obj_t *lid = lv_obj_create(st.root);
        lv_obj_remove_style_all(lid);
        lv_obj_set_size(lid, w, 5);
        lv_obj_set_pos(lid, x - w / 2, y - 2);
        lv_obj_set_style_radius(lid, 3, 0);
        lv_obj_set_style_bg_color(lid, lv_color_hex(C_PUPIL), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
    }
}

static void go(step_t s)
{
    st.step = s;
    build_root();
    switch (s) {
    case ST_HELLO:
        sleepy_head();
        st.title = mk_label(st.root, &ilf_font_48, C_WOOL, "Привет!");
        lv_obj_align(st.title, LV_ALIGN_TOP_MID, 0, 196);
        st.hint = mk_label(st.root, &ilf_font_24, C_SOFT, "Я твой барашек.\nДавай знакомиться");
        lv_obj_align(st.hint, LV_ALIGN_TOP_MID, 0, 262);
        st.next_btn = pill_button(st.root, "Дальше", next_clicked);
        lv_obj_align(st.next_btn, LV_ALIGN_BOTTOM_MID, 0, -28);
        break;
    case ST_OWNER:
        st.text[0] = 0; st.letters = 0;
        build_name_step("Как тебя зовут?");
        break;
    case ST_SHEEP:
        strcpy(st.result.owner, st.text);
        st.text[0] = 0; st.letters = 0;
        build_name_step("А как зовут меня?");
        break;
    case ST_DONE: {
        strcpy(st.result.sheep, st.text);
        sleepy_head();
        char buf[96];
        lv_snprintf(buf, sizeof buf, "Готово, %s!", st.result.owner);
        st.title = mk_label(st.root, &ilf_font_32, C_WOOL, buf);
        lv_obj_align(st.title, LV_ALIGN_TOP_MID, 0, 200);
        lv_snprintf(buf, sizeof buf, "Я %s.\nСтукни меня о барашка\nдруга, и мы подружимся", st.result.sheep);
        st.hint = mk_label(st.root, &ilf_font_24, C_SOFT, buf);
        lv_obj_align(st.hint, LV_ALIGN_TOP_MID, 0, 248);
        st.next_btn = pill_button(st.root, "Начать", next_clicked);
        lv_obj_align(st.next_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
        break;
    }
    }
}

void ilf_ui_setup_start(ilf_ui_setup_cb done, void *user)
{
    memset(&st.result, 0, sizeof st.result);
    st.done = done;
    st.done_user = user;
    go(ST_HELLO);
}

bool ilf_ui_setup_active(void) { return st.root != NULL; }
