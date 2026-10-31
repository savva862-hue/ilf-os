/* Тесты обновления от барашка к барашку: два барашка и радио с потерями. */
#include "ilf_ota.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, total = 0;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t mac[6]; uint8_t d[ILF_OTA_MAX_FRAME]; size_t n; } pkt_t;

typedef struct sheep {
    ilf_ota_t ota;
    uint8_t mac[6];
    uint8_t *image; uint32_t image_size;       /* своя прошивка */
    uint8_t *got;   uint32_t got_len, got_cap; /* принятая */
    int offers, done, failed;
    bool online, busy_radio;
    pkt_t inbox[256]; int nin;
} sheep_t;

static sheep_t A, B;
static unsigned loss_pct;
static uint32_t rnd = 12345;
static unsigned rand8(void) { rnd = rnd * 1103515245u + 12345u; return (rnd >> 16) % 100; }

static void toy_sha(const uint8_t *d, size_t n, uint8_t out[32])
{
    for (int k = 0; k < 4; k++) {
        uint64_t x = 1469598103934665603ULL + (uint64_t)k;
        for (size_t i = 0; i < n; i++) { x ^= d[i]; x *= 1099511628211ULL; }
        memcpy(out + k * 8, &x, 8);
    }
}

static sheep_t *other(sheep_t *s) { return s == &A ? &B : &A; }

static bool io_send(void *u, const uint8_t mac[6], const uint8_t *d, size_t n)
{
    sheep_t *s = u, *to = other(s);
    if (s->busy_radio && rand8() < 30) return false;     /* очередь радио полна */
    if (memcmp(mac, to->mac, 6) != 0 || !to->online) return true;
    if (rand8() < loss_pct) return true;                 /* пакет потерялся */
    if (to->nin < 256) { memcpy(to->inbox[to->nin].mac, s->mac, 6); memcpy(to->inbox[to->nin].d, d, n); to->inbox[to->nin].n = n; to->nin++; }
    return true;
}
static bool io_read(void *u, uint32_t off, uint8_t *buf, size_t n) { sheep_t *s = u; memcpy(buf, s->image + off, n); return true; }
static bool io_begin(void *u, uint32_t size) { sheep_t *s = u; s->got = malloc(size); s->got_cap = size; s->got_len = 0; return s->got != NULL; }
static bool io_write(void *u, const uint8_t *d, size_t n)
{
    sheep_t *s = u;
    if (s->got_len + n > s->got_cap) return false;
    memcpy(s->got + s->got_len, d, n); s->got_len += (uint32_t)n; return true;
}
static bool io_finish(void *u, const uint8_t sha[32]) { sheep_t *s = u; uint8_t h[32]; toy_sha(s->got, s->got_len, h); return memcmp(h, sha, 32) == 0; }
static void io_abort(void *u) { sheep_t *s = u; free(s->got); s->got = NULL; }
static void io_event(void *u, const ilf_ota_event_t *ev)
{
    sheep_t *s = u;
    if (ev->type == ILF_OTA_EV_OFFER) s->offers++;
    if (ev->type == ILF_OTA_EV_DONE) s->done++;
    if (ev->type == ILF_OTA_EV_FAILED) s->failed++;
}

static void setup(sheep_t *s, uint8_t id, uint16_t version, uint32_t size)
{
    free(s->image); free(s->got);
    memset(s, 0, sizeof *s);
    memset(s->mac, id, 6);
    s->online = true;
    s->image_size = size;
    s->image = malloc(size);
    for (uint32_t i = 0; i < size; i++) s->image[i] = (uint8_t)(i * 31 + id + (i >> 8));
    ilf_ota_image_t img = { .version = version, .size = size };
    toy_sha(s->image, size, img.sha);
    ilf_ota_io_t io = { io_send, io_read, io_begin, io_write, io_finish, io_abort, io_event, s };
    ilf_ota_init(&s->ota, &io, &img);
}

static void deliver(sheep_t *s, uint32_t now)
{
    pkt_t in[256]; int n = s->nin;
    memcpy(in, s->inbox, sizeof(pkt_t) * (size_t)n);
    s->nin = 0;
    for (int i = 0; i < n; i++) ilf_ota_on_recv(&s->ota, in[i].mac, in[i].d, in[i].n, now);
}

static uint32_t run(uint32_t now, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 33) {
        now += 33;
        deliver(&A, now); deliver(&B, now);
        ilf_ota_tick(&A.ota, now); ilf_ota_tick(&B.ota, now);
    }
    return now;
}

static void test_update_with_losses(void)
{
    setup(&A, 0xA1, 4, 100000);   /* у A новее */
    setup(&B, 0xB2, 3, 90000);
    loss_pct = 15;
    B.busy_radio = true;
    A.busy_radio = true;
    uint32_t now = 1000;
    ilf_ota_seen(&B.ota, A.mac, now);   /* B услышал A и сказал свою версию */
    now = run(now, 200);
    CHECK(B.offers == 1);
    CHECK(A.offers == 0);               /* у B старее — A ничего не предлагают */
    ilf_ota_accept(&B.ota, now);
    CHECK(ilf_ota_busy(&B.ota));
    now = run(now, 120000);
    CHECK(B.done == 1);
    CHECK(B.failed == 0);
    CHECK(B.got_len == 100000 && memcmp(B.got, A.image, 100000) == 0);
    CHECK(B.ota.state == ILF_OTA_READY);
    CHECK(!ilf_ota_busy(&A.ota));
    /* повторная встреча через минуту — не спамим версией */
    int before = B.nin;
    ilf_ota_seen(&A.ota, B.mac, now + 60000);
    CHECK(B.nin == before);
    B.busy_radio = A.busy_radio = false;
}

static void test_decline_and_same_version(void)
{
    setup(&A, 0xA1, 5, 5000);
    setup(&B, 0xB2, 4, 5000);
    loss_pct = 0;
    uint32_t now = 1;
    ilf_ota_seen(&A.ota, B.mac, now);
    now = run(now, 100);
    CHECK(B.offers == 1);
    ilf_ota_decline(&B.ota, true);
    /* через 11 минут снова встретились — эту версию уже не предлагаем */
    ilf_ota_seen(&A.ota, B.mac, now + 11 * 60 * 1000);
    now = run(now + 11 * 60 * 1000, 100);
    CHECK(B.offers == 1);

    setup(&A, 0xA1, 4, 5000);
    setup(&B, 0xB2, 4, 5000);
    ilf_ota_seen(&A.ota, B.mac, 1);
    run(1, 200);
    CHECK(A.offers == 0 && B.offers == 0);
}

static void test_not_now(void)
{
    setup(&A, 0xA1, 5, 5000);
    setup(&B, 0xB2, 4, 5000);
    loss_pct = 0;
    ilf_ota_seen(&A.ota, B.mac, 1);
    uint32_t now = run(1, 100);
    ilf_ota_decline(&B.ota, false);
    CHECK(B.ota.state == ILF_OTA_IDLE);
    ilf_ota_seen(&A.ota, B.mac, now + 11 * 60 * 1000);
    run(now + 11 * 60 * 1000, 100);
    CHECK(B.offers == 2);
}

static void test_friend_walks_away(void)
{
    setup(&A, 0xA1, 6, 200000);
    setup(&B, 0xB2, 5, 1000);
    loss_pct = 0;
    uint32_t now = 1;
    ilf_ota_seen(&A.ota, B.mac, now);
    now = run(now, 100);
    ilf_ota_accept(&B.ota, now);
    now = run(now, 300);
    CHECK(B.got_len > 0 && B.got_len < 200000);
    A.online = false; B.online = false;                 /* разошлись */
    now = run(now, (ILF_OTA_MAX_RETRIES + 2) * ILF_OTA_RETRY_MS + 1000);
    CHECK(B.failed == 1 && B.done == 0);
    CHECK(B.ota.state == ILF_OTA_IDLE && B.got == NULL);
}

static void test_bad_image(void)
{
    setup(&A, 0xA1, 7, 3000);
    setup(&B, 0xB2, 6, 1000);
    A.ota.mine.sha[0] ^= 1;     /* хеш не сходится с образом */
    loss_pct = 0;
    uint32_t now = 1;
    ilf_ota_seen(&A.ota, B.mac, now);
    now = run(now, 100);
    ilf_ota_accept(&B.ota, now);
    run(now, 3000);
    CHECK(B.failed == 1 && B.done == 0);
}

int main(void)
{
    test_update_with_losses();
    test_decline_and_same_version();
    test_not_now();
    test_friend_walks_away();
    test_bad_image();
    printf("ota: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
