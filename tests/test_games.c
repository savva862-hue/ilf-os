/* Тесты игр: лабиринт всегда проходим, шарик не проходит сквозь стены, доходит до выхода. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ilf_games.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_always_solvable(void)
{
    for (uint8_t lvl = 1; lvl <= 8; lvl++) {
        for (uint32_t seed = 1; seed < 200; seed++) {
            ilf_maze_t m;
            ilf_maze_new(&m, lvl, seed * 2654435761u);
            CHECK(ilf_maze_solvable(&m));
            CHECK(m.cols <= ILF_MAZE_MAX_C && m.rows <= ILF_MAZE_MAX_R);
            CHECK(ilf_maze_width(&m) <= 310.f && ilf_maze_height(&m) <= 352.f);
        }
    }
}

static void test_levels_grow(void)
{
    ilf_maze_t a, b;
    ilf_maze_new(&a, 1, 5);
    ilf_maze_new(&b, 4, 5);
    CHECK(b.cols * b.rows > a.cols * a.rows);
}

/* Клетка шарика может меняться только через открытый проход. */
static void test_no_tunneling(void)
{
    for (uint32_t seed = 1; seed < 40; seed++) {
        ilf_maze_t m;
        ilf_maze_new(&m, 1 + seed % 6, seed);
        uint32_t rng = seed;
        int pc = 0, pr = 0;
        for (int i = 0; i < 3000; i++) {
            rng = rng * 1103515245u + 12345u;
            float ax = (float)((rng >> 8) % 2001) / 1000.f - 1.f;
            float ay = (float)((rng >> 18) % 2001) / 1000.f - 1.f;
            ilf_maze_step(&m, ax * 1.5f, ay * 1.5f, 1.f / 30.f);
            if (m.won) break;
            int c = (int)(m.x / m.cell), r = (int)(m.y / m.cell);
            if (c != pc || r != pr) {
                bool ok = false;
                if (r == pr && c == pc + 1) ok = !m.v[r][c];
                if (r == pr && c == pc - 1) ok = !m.v[r][pc];
                if (c == pc && r == pr + 1) ok = !m.h[r][c];
                if (c == pc && r == pr - 1) ok = !m.h[pr][c];
                CHECK(ok);
                if (!ok) { printf("  seed %u step %d: (%d,%d)->(%d,%d)\n", seed, i, pr, pc, r, c); return; }
                pc = c; pr = r;
            }
        }
    }
}

/* Ведём шарик по решению: наклоняем к центру следующей клетки пути. */
static void test_reaches_goal(void)
{
    ilf_maze_t m;
    ilf_maze_new(&m, 3, 777);
    /* путь BFS от выхода: для каждой клетки — следующая к выходу */
    int nxt[ILF_MAZE_MAX_R * ILF_MAZE_MAX_C];
    for (int i = 0; i < ILF_MAZE_MAX_R * ILF_MAZE_MAX_C; i++) nxt[i] = -1;
    int q[ILF_MAZE_MAX_R * ILF_MAZE_MAX_C], h = 0, t = 0;
    int goal = (m.rows - 1) * m.cols + m.cols - 1;
    q[t++] = goal; nxt[goal] = goal;
    while (h < t) {
        int cur = q[h++], r = cur / m.cols, c = cur % m.cols;
        int nb[4][3] = {
            { r - 1, c, r > 0 && !m.h[r][c] }, { r + 1, c, r < m.rows - 1 && !m.h[r + 1][c] },
            { r, c - 1, c > 0 && !m.v[r][c] }, { r, c + 1, c < m.cols - 1 && !m.v[r][c + 1] },
        };
        for (int i = 0; i < 4; i++) {
            int id = nb[i][0] * m.cols + nb[i][1];
            if (!nb[i][2] || nxt[id] >= 0) continue;
            nxt[id] = cur; q[t++] = id;
        }
    }
    bool won = false;
    for (int i = 0; i < 30 * 60 && !won; i++) {
        int c = (int)(m.x / m.cell), r = (int)(m.y / m.cell);
        int to = nxt[r * m.cols + c];
        float tx = (to % m.cols + 0.5f) * m.cell, ty = (to / m.cols + 0.5f) * m.cell;
        /* целимся в центр следующей клетки и тормозим скоростью */
        float ax = (tx - m.x) * 0.02f - m.vx * 0.004f;
        float ay = (ty - m.y) * 0.02f - m.vy * 0.004f;
        won = ilf_maze_step(&m, fmaxf(-1, fminf(1, ax)), fmaxf(-1, fminf(1, ay)), 1.f / 30.f);
    }
    CHECK(won && m.won);
}

static void test_8ball(void)
{
    uint32_t rng = 42;
    bool yes = false, no = false, later = false;
    for (int i = 0; i < 500; i++) {
        const char *a = ilf_8ball_answer(&rng);
        CHECK(a && *a);
        if (!strcmp(a, "Да")) yes = true;
        if (!strcmp(a, "Нет")) no = true;
        if (!strcmp(a, "Спроси позже")) later = true;
    }
    CHECK(yes && no && later);
}

int main(void)
{
    test_always_solvable();
    test_levels_grow();
    test_no_tunneling();
    test_reaches_goal();
    test_8ball();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("games: all tests passed\n");
    return 0;
}
