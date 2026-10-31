/* Тесты стада и сжатия голоса: несколько барашков в эмулированном эфире. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ilf_adpcm.h"
#include "ilf_herd.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---------- игрушечная криптография: DH по модулю 2^61-1, хеш FNV ----------
   Настоящие X25519 и SHA-256 даёт mbedtls на плате; здесь проверяем только логику. */
#define P61 ((1ULL << 61) - 1)
static uint64_t mulmod(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) % P61); }
static uint64_t powmod(uint64_t g, uint64_t e)
{
    uint64_t r = 1;
    while (e) { if (e & 1) r = mulmod(r, g); g = mulmod(g, g); e >>= 1; }
    return r;
}
static uint64_t rng_state = 88172645463325252ULL;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }

static void t_keypair(void *ctx, uint8_t priv[32], uint8_t pub[32])
{
    (void)ctx;
    uint64_t a = rnd() % (P61 - 2) + 1, A = powmod(3, a);
    memset(priv, 0, 32); memset(pub, 0, 32);
    memcpy(priv, &a, 8); memcpy(pub, &A, 8);
}
static bool t_shared(void *ctx, const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32])
{
    (void)ctx;
    uint64_t a, B;
    memcpy(&a, priv, 8); memcpy(&B, pub, 8);
    uint64_t s = powmod(B, a);
    memset(out, 0, 32); memcpy(out, &s, 8);
    return true;
}
static void t_sha(void *ctx, const uint8_t *d, size_t n, uint8_t out[32])
{
    (void)ctx;
    for (int k = 0; k < 4; k++) {
        uint64_t x = 1469598103934665603ULL ^ (uint64_t)k;
        for (size_t i = 0; i < n; i++) { x ^= d[i]; x *= 1099511628211ULL; }
        memcpy(out + k * 8, &x, 8);
    }
}

/* ---------- эфир ---------- */
#define NODES 3
#define AIR_Q 64

typedef struct {
    int idx;
    uint8_t mac[6];
    ilf_herd_t *h;
    int friends_added, timeouts, messages, strokes, sent_ok, sent_fail;
    uint8_t last_msg[ILF_HERD_MAX_MESSAGE];
    size_t last_len;
    ilf_stroke_t last_stroke;
    uint8_t peers[ILF_HERD_MAX_FRIENDS][6];
    int npeers;
} node_t;

typedef struct { int from; bool bcast; uint8_t to[6]; uint8_t data[250]; size_t len; } pkt_t;

static node_t nodes[NODES];
static pkt_t air[AIR_Q];
static int air_n;
static int loss_percent;     /* потери адресных пакетов */
static bool deaf[NODES];     /* узел «вне зоны» */
static uint32_t now;

static void t_send(void *ctx, const uint8_t *mac, const uint8_t *data, size_t len)
{
    node_t *n = ctx;
    if (air_n >= AIR_Q) return;
    pkt_t *p = &air[air_n++];
    p->from = n->idx; p->bcast = mac == NULL; if (mac) memcpy(p->to, mac, 6);
    memcpy(p->data, data, len); p->len = len;
}
static void t_add_peer(void *ctx, const uint8_t mac[6], const uint8_t key[16])
{
    (void)key; node_t *n = ctx;
    for (int i = 0; i < n->npeers; i++) if (!memcmp(n->peers[i], mac, 6)) return;
    memcpy(n->peers[n->npeers++], mac, 6);
}
static void t_event(void *ctx, const ilf_herd_event_t *ev)
{
    node_t *n = ctx;
    switch (ev->type) {
    case ILF_HERD_EV_FRIEND_ADDED: n->friends_added++; break;
    case ILF_HERD_EV_PAIR_TIMEOUT: n->timeouts++; break;
    case ILF_HERD_EV_MESSAGE: n->messages++; memcpy(n->last_msg, ev->data, ev->len); n->last_len = ev->len; break;
    case ILF_HERD_EV_STROKE: n->strokes++; n->last_stroke = *ev->stroke; break;
    case ILF_HERD_EV_SENT: if (ev->ok) n->sent_ok++; else n->sent_fail++; break;
    default: break;
    }
}

static void reset(int count)
{
    for (int i = 0; i < NODES; i++) if (nodes[i].h) ilf_herd_destroy(nodes[i].h);
    memset(nodes, 0, sizeof nodes);
    memset(deaf, 0, sizeof deaf);
    air_n = 0; loss_percent = 0; now = 1000;
    for (int i = 0; i < count; i++) {
        node_t *n = &nodes[i];
        n->idx = i;
        uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0x00, 0x10, (uint8_t)(i + 1) };
        memcpy(n->mac, mac, 6);
        ilf_herd_io_t io = { .ctx = n, .send = t_send, .add_peer = t_add_peer, .keypair = t_keypair,
                             .shared = t_shared, .sha256 = t_sha, .event = t_event };
        char name[20]; snprintf(name, sizeof name, "Барашек %c", (char)('1' + i));
        n->h = ilf_herd_create(&io, mac, name);
    }
}

/* доставить всё, что в эфире, и продвинуть время на 10 мс */
static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        while (air_n) {
            pkt_t p = air[0];
            memmove(air, air + 1, sizeof air[0] * (size_t)--air_n);
            bool ok = true;
            for (int i = 0; i < NODES; i++) {
                if (!nodes[i].h || i == p.from || deaf[i]) continue;
                if (p.bcast) { ilf_herd_on_recv(nodes[i].h, nodes[p.from].mac, p.data, p.len, now); continue; }
                if (memcmp(nodes[i].mac, p.to, 6)) continue;
                if ((int)(rnd() % 100) < loss_percent) { ok = false; continue; }
                ilf_herd_on_recv(nodes[i].h, nodes[p.from].mac, p.data, p.len, now);
            }
            if (!p.bcast) {
                bool reached = false;
                for (int i = 0; i < NODES; i++) if (nodes[i].h && !deaf[i] && !memcmp(nodes[i].mac, p.to, 6)) reached = true;
                ok = ok && reached;
            }
            ilf_herd_on_sent(nodes[p.from].h, ok, now);
        }
        now += 10;
        for (int i = 0; i < NODES; i++) if (nodes[i].h) ilf_herd_tick(nodes[i].h, now);
    }
}

static void pair_ab(void)
{
    ilf_herd_bump(nodes[0].h, now);
    run(80);
    ilf_herd_bump(nodes[1].h, now);
    run(2500);
}

/* ---------- тесты ---------- */

static void test_bump_pairs_two(void)
{
    reset(3);
    pair_ab();
    CHECK(nodes[0].friends_added == 1 && nodes[1].friends_added == 1);
    CHECK(ilf_herd_friend_count(nodes[0].h) == 1 && ilf_herd_friend_count(nodes[1].h) == 1);
    const ilf_friend_t *fa = ilf_herd_friend(nodes[0].h, 0), *fb = ilf_herd_friend(nodes[1].h, 0);
    CHECK(fa && fb && memcmp(fa->key, fb->key, 16) == 0);       /* одинаковый ключ у обоих */
    CHECK(fa && memcmp(fa->mac, nodes[1].mac, 6) == 0);
    CHECK(fa && strcmp(fa->name, "Барашек 2") == 0);
    CHECK(nodes[0].npeers == 1 && nodes[1].npeers == 1);           /* зарегистрированы в радио */
    /* третий слышал всё, но не стукался — он не друг */
    CHECK(ilf_herd_friend_count(nodes[2].h) == 0 && nodes[2].friends_added == 0);
    CHECK(!ilf_herd_pairing(nodes[0].h));
}

static void test_bumps_far_apart_do_not_pair(void)
{
    reset(2);
    ilf_herd_bump(nodes[0].h, now);
    run(1200);
    ilf_herd_bump(nodes[1].h, now);
    run(3000);
    CHECK(ilf_herd_friend_count(nodes[0].h) == 0 && ilf_herd_friend_count(nodes[1].h) == 0);
    CHECK(nodes[0].timeouts == 1 && nodes[1].timeouts == 1);
}

static void test_bump_again_keeps_one_friend(void)
{
    reset(2);
    pair_ab();
    pair_ab();
    CHECK(ilf_herd_friend_count(nodes[0].h) == 1);
    CHECK(nodes[0].friends_added == 2);
    CHECK(!memcmp(ilf_herd_friend(nodes[0].h, 0)->key, ilf_herd_friend(nodes[1].h, 0)->key, 16));
}

static void test_voice_delivered_with_loss(void)
{
    reset(2);
    pair_ab();
    static uint8_t msg[ILF_HERD_CHUNK_DATA * 40 + 17];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 7 + 3);
    loss_percent = 25;
    uint16_t id = ilf_herd_send(nodes[0].h, 0, ILF_MSG_VOICE, msg, sizeof msg, now);
    CHECK(id != 0);
    CHECK(ilf_herd_send(nodes[0].h, 0, ILF_MSG_VOICE, msg, 10, now) == 0); /* одно сообщение за раз */
    run(5000);
    CHECK(nodes[1].messages == 1);
    CHECK(nodes[1].last_len == sizeof msg && memcmp(nodes[1].last_msg, msg, sizeof msg) == 0);
    CHECK(nodes[0].sent_ok == 1 && nodes[0].sent_fail == 0);
    CHECK(!ilf_herd_sending(nodes[0].h));
}

static void test_voice_to_whole_herd(void)
{
    reset(3);
    pair_ab();
    /* третий знакомится с первым */
    ilf_herd_bump(nodes[0].h, now);
    ilf_herd_bump(nodes[2].h, now + 30);
    deaf[1] = true;
    run(2500);
    deaf[1] = false;
    CHECK(ilf_herd_friend_count(nodes[0].h) == 2);
    uint8_t msg[500];
    memset(msg, 0x5A, sizeof msg);
    ilf_herd_send(nodes[0].h, ILF_HERD_ALL, ILF_MSG_VOICE, msg, sizeof msg, now);
    run(1000);
    CHECK(nodes[1].messages == 1 && nodes[2].messages == 1);
    CHECK(nodes[0].sent_ok == 2);
}

static void test_out_of_range_fails(void)
{
    reset(2);
    pair_ab();
    deaf[1] = true;
    uint8_t msg[300] = {1};
    ilf_herd_send(nodes[0].h, 0, ILF_MSG_VOICE, msg, sizeof msg, now);
    run(2000);
    CHECK(nodes[0].sent_fail == 1 && nodes[0].sent_ok == 0);
    CHECK(!ilf_herd_sending(nodes[0].h));
}

static void test_stroke(void)
{
    reset(2);
    pair_ab();
    ilf_stroke_t s = { .stroke_id = 7, .count = 3, .pts = { {10, 20}, {30, 40}, {367, 447} } };
    ilf_herd_send_stroke(nodes[0].h, ILF_HERD_ALL, &s, now);
    run(50);
    CHECK(nodes[1].strokes == 1);
    CHECK(nodes[1].last_stroke.stroke_id == 7 && nodes[1].last_stroke.count == 3);
    CHECK(nodes[1].last_stroke.pts[2].x == 367 && nodes[1].last_stroke.pts[2].y == 447);
}

static void test_stranger_ignored(void)
{
    reset(3);
    pair_ab();
    /* третий пытается подсунуть кусок сообщения первому */
    uint8_t fake[20] = { 0xF1, 1, 3, 1, 0, 0, 0, 1, 0, 1, 'h', 'i' };
    ilf_herd_on_recv(nodes[0].h, nodes[2].mac, fake, 12, now);
    CHECK(nodes[0].messages == 0);
    /* а от друга такой же пакет принимается */
    ilf_herd_on_recv(nodes[0].h, nodes[1].mac, fake, 12, now);
    CHECK(nodes[0].messages == 1 && nodes[0].last_len == 2);
}

static void test_forget_and_reload(void)
{
    reset(2);
    pair_ab();
    ilf_friend_t saved = *ilf_herd_friend(nodes[0].h, 0);
    ilf_herd_forget_friend(nodes[0].h, 0);
    CHECK(ilf_herd_friend_count(nodes[0].h) == 0);
    ilf_herd_load_friends(nodes[0].h, &saved, 1);
    CHECK(ilf_herd_friend_count(nodes[0].h) == 1);
    uint8_t msg[50] = {9};
    ilf_herd_send(nodes[1].h, 0, ILF_MSG_VOICE, msg, sizeof msg, now);
    run(200);
    CHECK(nodes[0].messages == 1);
}

static void test_adpcm(void)
{
    int16_t pcm[ILF_ADPCM_BLOCK_SAMPLES * 4], out[ILF_ADPCM_BLOCK_SAMPLES];
    for (int i = 0; i < ILF_ADPCM_BLOCK_SAMPLES * 4; i++) {
        pcm[i] = (int16_t)(9000 * sin(2 * M_PI * 440 * i / 8000.0) + 3000 * sin(2 * M_PI * 1300 * i / 8000.0));
    }
    ilf_adpcm_state_t st = {0};
    uint8_t blocks[4][ILF_ADPCM_BLOCK_BYTES];
    for (int b = 0; b < 4; b++) ilf_adpcm_encode_block(&st, pcm + b * ILF_ADPCM_BLOCK_SAMPLES, blocks[b]);
    CHECK(ILF_ADPCM_BLOCK_BYTES == ILF_HERD_CHUNK_DATA);
    /* третий блок раскодируется сам по себе, без первых двух */
    ilf_adpcm_decode_block(blocks[2], out);
    double sig = 0, err = 0;
    for (int i = 20; i < ILF_ADPCM_BLOCK_SAMPLES; i++) {
        double s = pcm[2 * ILF_ADPCM_BLOCK_SAMPLES + i];
        sig += s * s; err += (s - out[i]) * (s - out[i]);
    }
    double snr = 10 * log10(sig / (err + 1));
    CHECK(snr > 18.0);
    if (snr <= 18.0) printf("adpcm snr %.1f dB\n", snr);
}

int main(void)
{
    test_bump_pairs_two();
    test_bumps_far_apart_do_not_pair();
    test_bump_again_keeps_one_friend();
    test_voice_delivered_with_loss();
    test_voice_to_whole_herd();
    test_out_of_range_fails();
    test_stroke();
    test_stranger_ignored();
    test_forget_and_reload();
    test_adpcm();
    if (failures) { printf("%d herd check(s) failed\n", failures); return 1; }
    printf("all herd tests passed\n");
    return 0;
}
