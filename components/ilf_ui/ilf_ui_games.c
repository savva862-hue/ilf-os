/*
 * Экран «Игры»: «Лабиринт» (наклоняй барашка) и «Шар предсказаний» (встряхни).
 * Игра открывается поверх всего экрана, чтобы свайп не листал приложения.
 * Без датчика наклона (симулятор, тестер) шарик катится к пальцу.
 */
#include "ilf_ui_priv.h"

#include <math.h>
#include <stdio.h>

#include "ilf_games.h"

LV_FONT_DECLARE(ilf_font_24);
LV_FONT_DECLARE(ilf_font_32);
LV_FONT_DECLARE(ilf_font_48);

#define C_BG     0x000000
#define C_WOOL   0xF3EDE2
#define C_SOFT   0xE4DED4
#define C_MUTED  0x8F879B
#define C_KEY    0x262422
#define C_KEY_P  0x4A4642
#define C_WALL   0xE4DED4
#define C_EYE    0x121016
#define C_CLOVER 0x7BC47F
#define C_STEM   0x4E8F55
#define C_BALL8  0x1C1A2B
#define C_BALL8I 0x2E3270

#define MAZE_TOP 64
#define MAZE_H   372
#define PAD      2      /* поле шире лабиринта на полстенки с каждой стороны */

static struct {
    lv_obj_t *root;               /* открытая игра */
    lv_timer_t *timer;
    float tilt_x, tilt_y;         /* с датчика, доли g */
    bool finger;                  /* палец на поле — катим к нему */
    lv_point_t finger_pt;
    uint32_t last_tick;
    uint32_t rng;
    /* лабиринт */
    ilf_maze_t maze;
    lv_obj_t *field, *ball, *title, *banner;
    uint32_t banner_until;
    /* шар */
    lv_obj_t *sphere, *answer;
    uint32_t reveal_at;
    bool is_ball8;
} g;

/* ---------- помощники ---------- */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *dot(lv_obj_t *parent, int cx, int cy, int w, int h, uint32_t color)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, s);
    return l;
}

static void close_clicked(lv_event_t *e)
{
    (void)e;
    ilf_ui_game_close();
}

/* Полноэкранная панель игры с крестиком и заголовком. */
static lv_obj_t *game_root(const char *title)
{
    ilf_ui_game_close();
    lv_obj_t *r = g.root = box(lv_layer_top());
    lv_obj_set_size(r, ILF_SCREEN_W, ILF_SCREEN_H);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);   /* касания не уходят под панель */

    lv_obj_t *x = box(r);
    lv_obj_set_size(x, 52, 52);
    lv_obj_set_pos(x, 14, 8);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(C_KEY), 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(C_KEY_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(x, 10);
    lv_obj_add_event_cb(x, close_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(text(x, &ilf_font_32, C_WOOL, "×"));

    g.title = text(r, &ilf_font_32, C_WOOL, title);
    lv_obj_align(g.title, LV_ALIGN_TOP_MID, 0, 14);
    return r;
}

/* ---------- лабиринт ---------- */

static int field_x(void) { return (ILF_SCREEN_W - (int)ilf_maze_width(&g.maze)) / 2; }
static int field_y(void) { return MAZE_TOP + (MAZE_H - (int)ilf_maze_height(&g.maze)) / 2; }

static void maze_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(g.field, &a);
    a.x1 += PAD;
    a.y1 += PAD;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(C_WALL);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 2;
    const ilf_maze_t *m = &g.maze;
    int w = (int)(ILF_MAZE_WALL / 2), cell = (int)m->cell;
    for (int r = 0; r <= m->rows; r++) {
        for (int c = 0; c < m->cols; c++) {
            if (!m->h[r][c]) continue;
            lv_area_t q = { a.x1 + c * cell - w, a.y1 + r * cell - w, a.x1 + (c + 1) * cell + w - 1, a.y1 + r * cell + w - 1 };
            lv_draw_rect(layer, &d, &q);
        }
    }
    for (int r = 0; r < m->rows; r++) {
        for (int c = 0; c <= m->cols; c++) {
            if (!m->v[r][c]) continue;
            lv_area_t q = { a.x1 + c * cell - w, a.y1 + r * cell - w, a.x1 + c * cell + w - 1, a.y1 + (r + 1) * cell + w - 1 };
            lv_draw_rect(layer, &d, &q);
        }
    }
}

static void field_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *in = lv_indev_active();
        if (!in) return;
        lv_indev_get_point(in, &g.finger_pt);
        g.finger = true;
    } else {
        g.finger = false;
    }
}

static void place_ball(void)
{
    lv_obj_set_pos(g.ball, PAD + (int)(g.maze.x - g.maze.r), PAD + (int)(g.maze.y - g.maze.r));
}

static void maze_build(uint8_t level)
{
    ilf_maze_new(&g.maze, level, g.rng ^ (lv_tick_get() * 2654435761u));
    g.rng = g.maze.rng;
    if (g.field) lv_obj_delete(g.field);

    const ilf_maze_t *m = &g.maze;
    g.field = box(g.root);
    lv_obj_set_size(g.field, (int)ilf_maze_width(m) + 4, (int)ilf_maze_height(m) + 4);
    lv_obj_set_pos(g.field, field_x() - PAD, field_y() - PAD);
    lv_obj_set_style_pad_all(g.field, 0, 0);
    lv_obj_add_flag(g.field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.field, maze_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(g.field, field_touch, LV_EVENT_ALL, NULL);

    /* выход — клевер */
    int cell = (int)m->cell;
    int gx = PAD + (m->cols - 1) * cell + cell / 2, gy = PAD + (m->rows - 1) * cell + cell / 2;
    int lf = cell * 30 / 100;
    dot(g.field, gx - lf / 2, gy - lf / 3, lf, lf, C_CLOVER);
    dot(g.field, gx + lf / 2, gy - lf / 3, lf, lf, C_CLOVER);
    dot(g.field, gx, gy + lf / 2, lf, lf, C_CLOVER);
    dot(g.field, gx, gy + lf / 12, lf / 3, lf / 3, C_STEM);

    /* шарик — маленький барашек: кремовая головка и два глаза */
    int d = (int)(m->r * 2);
    g.ball = dot(g.field, 0, 0, d, d, C_WOOL);
    int ew = d / 6 > 2 ? d / 6 : 2, eh = d / 3;
    dot(g.ball, d * 32 / 100, d * 48 / 100, ew, eh, C_EYE);
    dot(g.ball, d * 68 / 100, d * 48 / 100, ew, eh, C_EYE);
    place_ball();

    char buf[48];
    snprintf(buf, sizeof buf, "Лабиринт · %u", (unsigned)level);
    lv_label_set_text(g.title, buf);
}

static void maze_tick(lv_timer_t *t)
{
    (void)t;
    if (!g.root || !g.field) return;
    uint32_t now = lv_tick_get();
    float dt = (float)(uint32_t)(now - g.last_tick) / 1000.f;
    g.last_tick = now;

    if (g.banner) {
        if ((int32_t)(now - g.banner_until) < 0) return;
        lv_obj_delete(g.banner);
        g.banner = NULL;
        maze_build((uint8_t)(g.maze.level + 1));
        return;
    }

    float ax = g.tilt_x, ay = g.tilt_y;
    if (g.finger) {
        /* к пальцу: направление от шарика, сила растёт с расстоянием */
        lv_area_t a;
        lv_obj_get_coords(g.field, &a);
        float dx = (float)(g.finger_pt.x - a.x1 - PAD) - g.maze.x;
        float dy = (float)(g.finger_pt.y - a.y1 - PAD) - g.maze.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 4.f) {
            float k = fminf(len / 80.f, 1.f) * 0.9f / len;
            ax = dx * k;
            ay = dy * k;
        }
    }
    bool won = ilf_maze_step(&g.maze, ax, ay, dt);
    place_ball();
    if (won) {
        char buf[48];
        snprintf(buf, sizeof buf, "Ура!\nУровень %u", (unsigned)(g.maze.level + 1));
        g.banner = text(g.root, &ilf_font_48, C_WOOL, buf);
        lv_obj_set_style_bg_color(g.banner, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(g.banner, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(g.banner, 16, 0);
        lv_obj_set_style_radius(g.banner, 20, 0);
        lv_obj_center(g.banner);
        g.banner_until = now + 1400;
    }
}

static void open_maze(lv_event_t *e)
{
    (void)e;
    game_root("Лабиринт");
    g.is_ball8 = false;
    g.field = NULL;
    maze_build(1);
    g.last_tick = lv_tick_get();
    g.timer = lv_timer_create(maze_tick, 33, NULL);
}

/* ---------- шар предсказаний ---------- */

static void ball8_tick(lv_timer_t *t)
{
    (void)t;
    if (!g.root || !g.reveal_at) return;
    if ((int32_t)(lv_tick_get() - g.reveal_at) < 0) return;
    g.reveal_at = 0;
    lv_label_set_text(g.answer, ilf_8ball_answer(&g.rng));
    lv_obj_fade_in(g.answer, 400, 0);
}

static void sphere_x(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, (ILF_SCREEN_W - 270) / 2 + v);
}

static void ball8_ask(void)
{
    if (!g.root || !g.is_ball8 || g.reveal_at) return;
    lv_label_set_text(g.answer, "…");
    /* шар трясётся, потом всплывает ответ */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g.sphere);
    lv_anim_set_exec_cb(&a, sphere_x);
    lv_anim_set_values(&a, -14, 14);
    lv_anim_set_duration(&a, 90);
    lv_anim_set_playback_duration(&a, 90);
    lv_anim_set_repeat_count(&a, 3);
    lv_anim_set_completed_cb(&a, NULL);
    lv_anim_start(&a);
    g.reveal_at = lv_tick_get() + 800;
    if (!g.reveal_at) g.reveal_at = 1;
}

static void sphere_clicked(lv_event_t *e)
{
    (void)e;
    ball8_ask();
}

static void open_ball8(lv_event_t *e)
{
    (void)e;
    lv_obj_t *r = game_root("Шар предсказаний");
    lv_obj_set_style_text_font(g.title, &ilf_font_24, 0);   /* длинное — мельче, чтобы не залезть на крестик */
    lv_obj_align(g.title, LV_ALIGN_TOP_MID, 24, 20);
    g.is_ball8 = true;
    g.reveal_at = 0;
    g.sphere = dot(r, ILF_SCREEN_W / 2, 220, 270, 270, C_BALL8);
    lv_obj_set_style_border_color(g.sphere, lv_color_hex(0x3A3650), 0);
    lv_obj_set_style_border_width(g.sphere, 3, 0);
    lv_obj_add_flag(g.sphere, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.sphere, sphere_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *inner = dot(g.sphere, 132, 132, 170, 170, C_BALL8I);
    g.answer = text(inner, &ilf_font_32, C_WOOL, "?");
    lv_obj_set_width(g.answer, 150);
    lv_label_set_long_mode(g.answer, LV_LABEL_LONG_WRAP);
    lv_obj_center(g.answer);
    lv_obj_t *hint = text(r, &ilf_font_24, C_SOFT, "Задай вопрос и встряхни барашка");
    lv_obj_set_width(hint, 320);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 372);
    g.timer = lv_timer_create(ball8_tick, 50, NULL);
}

/* ---------- плитка «Игры» ---------- */

static lv_obj_t *game_card(lv_obj_t *tile, int y, const char *name, const char *sub, lv_event_cb_t cb)
{
    lv_obj_t *c = box(tile);
    lv_obj_set_size(c, 312, 116);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(c, 28, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_KEY), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_KEY_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *n = text(c, &ilf_font_32, C_WOOL, name);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *s = text(c, &ilf_font_24, C_MUTED, sub);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 66);
    return c;
}

void ilf_ui_build_games(lv_obj_t *tile)
{
    g.rng = 0x51EE7u;
    lv_obj_t *t = text(tile, &ilf_font_32, C_WOOL, "Игры");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    game_card(tile, 92, "Лабиринт", "наклоняй барашка", open_maze);
    game_card(tile, 232, "Шар предсказаний", "встряхни — ответит", open_ball8);
}

/* ---------- снаружи ---------- */

void ilf_ui_tilt(float ax, float ay)
{
    g.tilt_x = ax;
    g.tilt_y = ay;
}

void ilf_ui_shake(void)
{
    if (g.root && g.is_ball8) ball8_ask();
}

bool ilf_ui_game_active(void) { return g.root != NULL; }

void ilf_ui_game_close(void)
{
    if (g.timer) { lv_timer_delete(g.timer); g.timer = NULL; }
    if (g.root) {
        if (g.sphere) lv_anim_delete(g.sphere, NULL);
        lv_obj_delete(g.root);
    }
    g.root = g.field = g.ball = g.title = g.banner = g.sphere = g.answer = NULL;
    g.finger = false;
    g.reveal_at = 0;
}

/* для симулятора: открыть игру сразу */
void ilf_ui_game_open(int which)
{
    if (which == 0) open_maze(NULL);
    else open_ball8(NULL);
}
