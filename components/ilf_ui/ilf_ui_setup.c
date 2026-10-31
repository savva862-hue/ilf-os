/*
 * Первый запуск и замок.
 *
 * Экран крошечный (1.8"), поэтому клавиатура не «йцукен» в 11 клавиш, а крупная:
 * 12 букв на странице (4×3) и три страницы алфавита. Первая буква имени — заглавная,
 * остальные — строчные, сами.
 */
#include "ilf_ui_priv.h"

#include <string.h>

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

#define NAME_MAX_LETTERS 11

typedef enum { ST_HELLO, ST_OWNER, ST_SHEEP, ST_PIN, ST_PIN2, ST_DONE, ST_LOCK } step_t;

static struct {
    lv_obj_t *root;            /* полноэкранная панель поверх всего */
    lv_obj_t *title, *hint, *field, *field_lbl, *dots[ILF_PIN_LEN_UI], *msg;
    lv_obj_t *kbd, *pad, *next_btn, *head;
    step_t step;
    int page;
    char text[ILF_HERD_NAME_LEN];
    int letters;
    char pin[5], pin_first[5];
    ilf_setup_result_t result;
    ilf_ui_setup_cb done;
    void *done_user;
    /* замок */
    bool locked;
    ilf_ui_pin_cb pin_cb;
    void *pin_user;
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

/* ---------- клавиатура имён ---------- */

static const char *const pages[3][16] = {
    { "А", "Б", "В", "Г", "\n", "Д", "Е", "Ж", "З", "\n", "И", "Й", "К", "Л", "\n", NULL },
    { "М", "Н", "О", "П", "\n", "Р", "С", "Т", "У", "\n", "Ф", "Х", "Ц", "Ч", "\n", NULL },
    { "Ш", "Щ", "Ы", "Ь", "\n", "Э", "Ю", "Я", "Ё", "\n", "Ъ", "-", "пробел", "\n", NULL },
};
static const char *map_cache[3][24];

static const char **kbd_map(int page)
{
    const char **m = map_cache[page];
    int n = 0;
    for (int i = 0; pages[page][i]; i++) m[n++] = pages[page][i];
    /* нижний ряд: страницы алфавита, стереть, готово */
    m[n++] = "А…"; m[n++] = "М…"; m[n++] = "Ш…"; m[n++] = LV_SYMBOL_BACKSPACE; m[n++] = LV_SYMBOL_OK;
    m[n] = "";
    return m;
}

static void kbd_apply_page(void)
{
    lv_buttonmatrix_set_map(st.kbd, kbd_map(st.page));
    uint32_t letters = st.page == 2 ? 11 : 12;
    if (st.page == 2) lv_buttonmatrix_set_button_width(st.kbd, 10, 2);   /* «пробел» шире */
    /* подсветить текущую страницу */
    for (int i = 0; i < 3; i++) {
        uint32_t id = letters + (uint32_t)i;
        if (i == st.page) lv_buttonmatrix_set_button_ctrl(st.kbd, id, LV_BUTTONMATRIX_CTRL_CHECKED);
        else lv_buttonmatrix_clear_button_ctrl(st.kbd, id, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

static void field_refresh(void)
{
    lv_label_set_text_fmt(st.field_lbl, "%s|", st.text);
}

/* перевести заглавную русскую букву (UTF-8) в строчную */
static void to_lower_ru(const char *up, char *out)
{
    unsigned char a = (unsigned char)up[0], b = (unsigned char)up[1];
    if (a == 0xD0 && b == 0x81) { out[0] = (char)0xD1; out[1] = (char)0x91; out[2] = 0; return; }      /* Ё → ё */
    if (a == 0xD0 && b >= 0x90 && b <= 0x9F) { out[0] = (char)0xD0; out[1] = (char)(b + 0x20); out[2] = 0; return; } /* А–П */
    if (a == 0xD0 && b >= 0xA0 && b <= 0xAF) { out[0] = (char)0xD1; out[1] = (char)(b - 0x20); out[2] = 0; return; } /* Р–Я */
    strcpy(out, up);
}

static void name_backspace(void)
{
    size_t n = strlen(st.text);
    if (!n) return;
    n--;
    while (n > 0 && ((unsigned char)st.text[n] & 0xC0) == 0x80) n--;
    st.text[n] = 0;
    st.letters--;
}

static void go(step_t s);

static void kbd_event(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(st.kbd);
    const char *t = lv_buttonmatrix_get_button_text(st.kbd, id);
    (void)e;
    if (!t || !*t) return;
    if (!strcmp(t, "А…")) { st.page = 0; kbd_apply_page(); return; }
    if (!strcmp(t, "М…")) { st.page = 1; kbd_apply_page(); return; }
    if (!strcmp(t, "Ш…")) { st.page = 2; kbd_apply_page(); return; }
    if (!strcmp(t, LV_SYMBOL_BACKSPACE)) { name_backspace(); field_refresh(); return; }
    if (!strcmp(t, LV_SYMBOL_OK)) {
        /* убрать пробелы по краям */
        while (st.letters && st.text[strlen(st.text) - 1] == ' ') name_backspace();
        if (!st.letters) { lv_label_set_text(st.msg, "Нужна хотя бы одна буква"); return; }
        go(st.step == ST_OWNER ? ST_SHEEP : ST_PIN);
        return;
    }
    if (st.letters >= NAME_MAX_LETTERS) return;
    char add[8];
    if (!strcmp(t, "пробел")) { if (!st.letters) return; strcpy(add, " "); }
    else if (st.letters == 0 || st.text[strlen(st.text) - 1] == ' ' || st.text[strlen(st.text) - 1] == '-') strcpy(add, t);
    else to_lower_ru(t, add);
    if (strlen(st.text) + strlen(add) >= sizeof st.text) return;
    strcat(st.text, add);
    st.letters++;
    lv_label_set_text(st.msg, "");
    field_refresh();
}

/* ---------- цифры PIN ---------- */

static const char *pad_map[] = {
    "1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n", LV_SYMBOL_LEFT, "0", LV_SYMBOL_BACKSPACE, "",
};

static void dots_refresh(void)
{
    size_t n = strlen(st.pin);
    for (int i = 0; i < ILF_PIN_LEN_UI; i++) {
        lv_obj_set_style_bg_opa(st.dots[i], i < (int)n ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void pin_complete(void)
{
    if (st.step == ST_PIN) {
        strcpy(st.pin_first, st.pin);
        go(ST_PIN2);
    } else if (st.step == ST_PIN2) {
        if (strcmp(st.pin, st.pin_first) != 0) {
            go(ST_PIN);
            lv_label_set_text(st.msg, "Не совпало, давай ещё раз");
            return;
        }
        strcpy(st.result.pin, st.pin);
        go(ST_DONE);
    } else if (st.step == ST_LOCK) {
        char pin[5];
        strcpy(pin, st.pin);
        if (st.pin_cb) st.pin_cb(pin, st.pin_user);
    }
}

static void pad_event(lv_event_t *e)
{
    (void)e;
    uint32_t id = lv_buttonmatrix_get_selected_button(st.pad);
    const char *t = lv_buttonmatrix_get_button_text(st.pad, id);
    if (!t || !*t) return;
    size_t n = strlen(st.pin);
    if (!strcmp(t, LV_SYMBOL_BACKSPACE)) {
        if (n) st.pin[n - 1] = 0;
    } else if (!strcmp(t, LV_SYMBOL_LEFT)) {
        if (st.step == ST_LOCK) { ilf_ui_lock_hide(); return; }
        if (st.step == ST_PIN) { go(ST_SHEEP); return; }
        go(ST_PIN);
        return;
    } else if (n < ILF_PIN_LEN_UI) {
        st.pin[n] = t[0];
        st.pin[n + 1] = 0;
        lv_label_set_text(st.msg, "");
    }
    dots_refresh();
    if (strlen(st.pin) == ILF_PIN_LEN_UI) pin_complete();
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
    memset(st.dots, 0, sizeof st.dots);
    st.title = st.hint = st.field = st.field_lbl = st.msg = st.kbd = st.pad = st.next_btn = st.head = NULL;
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

static void build_pin_step(const char *title)
{
    st.title = mk_label(st.root, &ilf_font_32, C_WOOL, title);
    lv_obj_align(st.title, LV_ALIGN_TOP_MID, 0, 22);
    for (int i = 0; i < ILF_PIN_LEN_UI; i++) {
        lv_obj_t *d = lv_obj_create(st.root);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 22, 22);
        lv_obj_set_pos(d, ILF_SCREEN_W / 2 - 78 + i * 44, 76);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(d, lv_color_hex(C_WOOL), 0);
        lv_obj_set_style_border_width(d, 3, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(C_WOOL), 0);
        st.dots[i] = d;
    }
    st.msg = mk_label(st.root, &ilf_font_24, C_NOSE, "");
    lv_obj_align(st.msg, LV_ALIGN_TOP_MID, 0, 108);

    st.pad = lv_buttonmatrix_create(st.root);
    style_matrix(st.pad, &ilf_font_32);
    lv_buttonmatrix_set_map(st.pad, pad_map);
    lv_obj_set_size(st.pad, 330, 290);
    lv_obj_align(st.pad, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_event_cb(st.pad, pad_event, LV_EVENT_VALUE_CHANGED, NULL);
    st.pin[0] = 0;
    dots_refresh();
}

static void go(step_t s)
{
    st.step = s;
    build_root();
    switch (s) {
    case ST_HELLO:
        st.head = lv_image_create(st.root);
        lv_image_set_src(st.head, &ilf_head);
        lv_image_set_scale(st.head, 170);
        lv_obj_align(st.head, LV_ALIGN_TOP_MID, 0, -6);
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
    case ST_PIN:
        if (st.text[0]) { strcpy(st.result.sheep, st.text); st.text[0] = 0; st.letters = 0; }
        build_pin_step("Придумай PIN");
        break;
    case ST_PIN2:
        build_pin_step("Повтори PIN");
        break;
    case ST_DONE: {
        st.head = lv_image_create(st.root);
        lv_image_set_src(st.head, &ilf_head);
        lv_image_set_scale(st.head, 170);
        lv_obj_align(st.head, LV_ALIGN_TOP_MID, 0, -6);
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
    case ST_LOCK:
        build_pin_step("Введи PIN");
        break;
    }
}

void ilf_ui_setup_start(ilf_ui_setup_cb done, void *user)
{
    memset(&st.result, 0, sizeof st.result);
    st.done = done;
    st.done_user = user;
    go(ST_HELLO);
}

bool ilf_ui_setup_active(void) { return st.root && st.step != ST_LOCK; }

/* ---------- замок ---------- */

void ilf_ui_lock_show_pad(void)
{
    if (st.root) return;
    go(ST_LOCK);
}

void ilf_ui_lock_message(const char *text)
{
    if (!st.root || st.step != ST_LOCK) return;
    lv_label_set_text(st.msg, text ? text : "");
    st.pin[0] = 0;
    dots_refresh();
}

void ilf_ui_lock_hide(void)
{
    if (st.root && st.step == ST_LOCK) {
        lv_obj_delete(st.root);
        st.root = NULL;
    }
}

void ilf_ui_lock_state(bool locked, ilf_ui_pin_cb cb, void *user)
{
    st.locked = locked;
    st.pin_cb = cb;
    st.pin_user = user;
    if (!locked) ilf_ui_lock_hide();
}

bool ilf_ui_locked(void) { return st.locked; }
