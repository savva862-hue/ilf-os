#include "ilf_games.h"

#include <math.h>
#include <string.h>

static uint32_t next_rand(uint32_t *s)
{
    /* xorshift32 */
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Размеры по уровням: лабиринт растёт, клетки мельчают. Поле ~304×352 px. */
static void level_size(uint8_t level, uint8_t *cols, uint8_t *rows, float *cell)
{
    static const struct { uint8_t c, r; float cell; } sz[] = {
        { 4, 5, 70 }, { 5, 6, 58 }, { 6, 7, 50 }, { 7, 8, 43 }, { 8, 9, 38 }, { 9, 10, 34 },
    };
    unsigned i = level ? (unsigned)level - 1 : 0;
    if (i >= sizeof sz / sizeof sz[0]) i = sizeof sz / sizeof sz[0] - 1;
    *cols = sz[i].c;
    *rows = sz[i].r;
    *cell = sz[i].cell;
}

void ilf_maze_new(ilf_maze_t *m, uint8_t level, uint32_t seed)
{
    memset(m, 0, sizeof *m);
    m->level = level ? level : 1;
    m->rng = seed ? seed : 1;
    level_size(m->level, &m->cols, &m->rows, &m->cell);

    for (int r = 0; r <= m->rows; r++)
        for (int c = 0; c < m->cols; c++) m->h[r][c] = true;
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c <= m->cols; c++) m->v[r][c] = true;

    /* «поиск в глубину» со стеком: получается лабиринт без петель, путь всегда один */
    bool seen[ILF_MAZE_MAX_R][ILF_MAZE_MAX_C] = {{0}};
    uint8_t stack[ILF_MAZE_MAX_R * ILF_MAZE_MAX_C];
    int top = 0;
    stack[top++] = 0;
    seen[0][0] = true;
    while (top) {
        int cur = stack[top - 1];
        int r = cur / m->cols, c = cur % m->cols;
        int opts[4], n = 0;
        if (r > 0 && !seen[r - 1][c]) opts[n++] = 0;
        if (c < m->cols - 1 && !seen[r][c + 1]) opts[n++] = 1;
        if (r < m->rows - 1 && !seen[r + 1][c]) opts[n++] = 2;
        if (c > 0 && !seen[r][c - 1]) opts[n++] = 3;
        if (!n) { top--; continue; }
        int d = opts[next_rand(&m->rng) % (uint32_t)n];
        int nr = r, nc = c;
        switch (d) {
        case 0: m->h[r][c] = false; nr = r - 1; break;
        case 1: m->v[r][c + 1] = false; nc = c + 1; break;
        case 2: m->h[r + 1][c] = false; nr = r + 1; break;
        default: m->v[r][c] = false; nc = c - 1; break;
        }
        seen[nr][nc] = true;
        stack[top++] = (uint8_t)(nr * m->cols + nc);
    }

    m->r = m->cell * 0.26f;
    m->x = m->cell * 0.5f;
    m->y = m->cell * 0.5f;
}

float ilf_maze_width(const ilf_maze_t *m) { return m->cols * m->cell; }
float ilf_maze_height(const ilf_maze_t *m) { return m->rows * m->cell; }

/* Вытолкнуть шарик из прямоугольника стенки и погасить скорость в стенку. */
static void push_out(ilf_maze_t *m, float x0, float y0, float x1, float y1)
{
    float px = m->x < x0 ? x0 : (m->x > x1 ? x1 : m->x);
    float py = m->y < y0 ? y0 : (m->y > y1 ? y1 : m->y);
    float dx = m->x - px, dy = m->y - py;
    float d2 = dx * dx + dy * dy;
    if (d2 >= m->r * m->r) return;
    float d = sqrtf(d2);
    float nx, ny;
    if (d > 1e-4f) {
        nx = dx / d;
        ny = dy / d;
    } else {
        /* центр внутри стенки (не должно быть при мелких шагах) — толкаем по короткой оси */
        float l = m->x - x0, rr = x1 - m->x, t = m->y - y0, b = y1 - m->y;
        float mn = fminf(fminf(l, rr), fminf(t, b));
        nx = mn == l ? -1.f : (mn == rr ? 1.f : 0.f);
        ny = nx != 0.f ? 0.f : (mn == t ? -1.f : 1.f);
        d = 0.f;
    }
    float pen = m->r - d;
    m->x += nx * pen;
    m->y += ny * pen;
    float vn = m->vx * nx + m->vy * ny;
    if (vn < 0.f) {
        /* лёгкий отскок */
        m->vx -= 1.3f * vn * nx;
        m->vy -= 1.3f * vn * ny;
    }
}

static void collide(ilf_maze_t *m)
{
    const float w = ILF_MAZE_WALL * 0.5f;
    int cc = (int)(m->x / m->cell), cr = (int)(m->y / m->cell);
    for (int r = cr - 1; r <= cr + 1; r++) {
        for (int c = cc - 1; c <= cc + 1; c++) {
            if (r < 0 || c < 0 || r > m->rows || c > m->cols) continue;
            if (c < m->cols && m->h[r][c]) {
                float y = r * m->cell;
                push_out(m, c * m->cell - w, y - w, (c + 1) * m->cell + w, y + w);
            }
            if (r < m->rows && m->v[r][c]) {
                float x = c * m->cell;
                push_out(m, x - w, r * m->cell - w, x + w, (r + 1) * m->cell + w);
            }
        }
    }
}

bool ilf_maze_step(ilf_maze_t *m, float ax, float ay, float dt)
{
    if (m->won) return false;
    if (dt > 0.1f) dt = 0.1f;
    const float accel = 1100.f;      /* px/с² на 1 g */
    const float vmax = 520.f;
    m->vx += ax * accel * dt;
    m->vy += ay * accel * dt;
    float damp = 1.f - 1.2f * dt;    /* трение */
    m->vx *= damp;
    m->vy *= damp;
    float sp = sqrtf(m->vx * m->vx + m->vy * m->vy);
    if (sp > vmax) { m->vx *= vmax / sp; m->vy *= vmax / sp; }

    /* мелкими шагами, чтобы шарик не проскакивал сквозь стенки */
    float dist = sp * dt;
    int steps = (int)(dist / (m->r * 0.4f)) + 1;
    float sdt = dt / (float)steps;
    for (int i = 0; i < steps; i++) {
        m->x += m->vx * sdt;
        m->y += m->vy * sdt;
        collide(m);
    }
    /* на всякий случай не выпускаем за рамку */
    float W = ilf_maze_width(m), H = ilf_maze_height(m);
    if (m->x < m->r) m->x = m->r;
    if (m->y < m->r) m->y = m->r;
    if (m->x > W - m->r) m->x = W - m->r;
    if (m->y > H - m->r) m->y = H - m->r;

    int gc = m->cols - 1, gr = m->rows - 1;
    float gx = (gc + 0.5f) * m->cell, gy = (gr + 0.5f) * m->cell;
    float dx = m->x - gx, dy = m->y - gy;
    if (dx * dx + dy * dy < (m->cell * 0.3f) * (m->cell * 0.3f)) {
        m->won = true;
        return true;
    }
    return false;
}

bool ilf_maze_solvable(const ilf_maze_t *m)
{
    bool seen[ILF_MAZE_MAX_R][ILF_MAZE_MAX_C] = {{0}};
    uint8_t q[ILF_MAZE_MAX_R * ILF_MAZE_MAX_C];
    int head = 0, tail = 0, visited = 0;
    q[tail++] = 0;
    seen[0][0] = true;
    while (head < tail) {
        int cur = q[head++], r = cur / m->cols, c = cur % m->cols;
        visited++;
        int nb[4][3] = {
            { r - 1, c, r > 0 && !m->h[r][c] },
            { r + 1, c, r < m->rows - 1 && !m->h[r + 1][c] },
            { r, c - 1, c > 0 && !m->v[r][c] },
            { r, c + 1, c < m->cols - 1 && !m->v[r][c + 1] },
        };
        for (int i = 0; i < 4; i++) {
            if (!nb[i][2] || seen[nb[i][0]][nb[i][1]]) continue;
            seen[nb[i][0]][nb[i][1]] = true;
            q[tail++] = (uint8_t)(nb[i][0] * m->cols + nb[i][1]);
        }
    }
    /* все клетки достижимы, значит и выход тоже */
    return visited == m->cols * m->rows && seen[m->rows - 1][m->cols - 1];
}

const char *ilf_8ball_answer(uint32_t *rng)
{
    static const char *const answers[] = {
        "Да", "Нет", "Спроси позже",
        "Бе-е… да!", "Бе-е… нет", "Конечно", "Даже не думай",
        "Звёзды говорят да", "Сейчас не скажу", "Скорее да", "Скорее нет",
        "Спроси у стада",
    };
    return answers[next_rand(rng) % (sizeof answers / sizeof answers[0])];
}
