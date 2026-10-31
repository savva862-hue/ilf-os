/*
 * Игры ILF OS — чистый C без железа и LVGL, чтобы гонять в тестах.
 *
 * «Лабиринт»: наклоняешь барашка — катится шарик. Каждый уровень —
 * новый лабиринт, чуть больше прошлого. Выход — клевер в правом нижнем углу.
 *
 * «Шар предсказаний»: задал вопрос, встряхнул — ответ.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ILF_MAZE_MAX_C 9
#define ILF_MAZE_MAX_R 10
#define ILF_MAZE_WALL  4.0f     /* толщина стенки, px */

typedef struct {
    uint8_t cols, rows;
    float   cell;               /* размер клетки, px */
    /* h[r][c] — стенка над клеткой (r, c); r = rows — нижний край */
    bool    h[ILF_MAZE_MAX_R + 1][ILF_MAZE_MAX_C];
    /* v[r][c] — стенка слева от клетки (r, c); c = cols — правый край */
    bool    v[ILF_MAZE_MAX_R][ILF_MAZE_MAX_C + 1];
    float   x, y, vx, vy;       /* шарик в координатах лабиринта, px */
    float   r;                  /* радиус шарика */
    uint8_t level;
    bool    won;
    uint32_t rng;
} ilf_maze_t;

/* Новый лабиринт для уровня (1, 2, …). Шарик — в левой верхней клетке. */
void ilf_maze_new(ilf_maze_t *m, uint8_t level, uint32_t seed);
/* Ширина и высота лабиринта в px. */
float ilf_maze_width(const ilf_maze_t *m);
float ilf_maze_height(const ilf_maze_t *m);
/* Шаг физики. ax, ay — наклон в долях g (-1..1), dt — секунды.
   Возвращает true в момент, когда шарик дошёл до выхода. */
bool ilf_maze_step(ilf_maze_t *m, float ax, float ay, float dt);
/* Есть ли путь от входа до выхода (для тестов: лабиринт всегда проходим). */
bool ilf_maze_solvable(const ilf_maze_t *m);

/* Шар предсказаний: ответ по случайному числу. */
const char *ilf_8ball_answer(uint32_t *rng);

#ifdef __cplusplus
}
#endif
