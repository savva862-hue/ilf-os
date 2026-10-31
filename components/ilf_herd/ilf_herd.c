#include "ilf_herd.h"

#include <stdlib.h>
#include <string.h>

/* ---- формат пакетов (всё little-endian) ----
 *  [0] 0xF1  [1] версия  [2] тип  [...] тело
 *  HELLO   : pub[32] bump_age_ms(u16) name[24]          — широковещательно
 *  CONFIRM : target_mac[6] proof[8]                     — широковещательно
 *  CHUNK   : msg_id(u16) idx(u16) total(u16) kind(u8) data[..229]   — другу, шифровано
 *  STROKE  : stroke_id(u8) flags(u8) n(u8) n×(x u16, y u16)         — другу, шифровано
 *  LIVE    : seq(u16) data[0..229]                                  — рация, другу, шифровано
 */
#define MAGIC       0xF1
#define VERSION     1
#define T_HELLO     1
#define T_CONFIRM   2
#define T_CHUNK     3
#define T_STROKE    4
#define T_LIVE      5
#define HDR         3
#define CHUNK_HDR   (HDR + 7)
#define MAX_CHUNKS  (ILF_HERD_MAX_MESSAGE / ILF_HERD_CHUNK_DATA)
#define CTRL_QUEUE  16
#define HELLO_SLOTS 4
#define TX_STUCK_MS 1000u

typedef struct {
    uint8_t mac[6];
    bool    broadcast;
    uint8_t len;
    uint8_t data[ILF_HERD_MAX_FRAME];
} frame_t;

typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint8_t  pub[32];
    char     name[ILF_HERD_NAME_LEN];
    uint32_t their_bump_ms;   /* в наших часах */
    uint32_t rx_ms;
    bool     matched;         /* уже посчитали общий ключ */
    uint8_t  key[16];
    uint8_t  confirms_left;
    bool     confirmed;       /* он прислал верное подтверждение */
} hello_t;

typedef struct {
    bool     active;
    uint16_t msg_id;
    uint16_t last_done_id;
    uint16_t total;
    uint16_t count;
    size_t   len;
    uint8_t  kind;
    uint32_t last_rx_ms;
    uint8_t  bitmap[(MAX_CHUNKS + 7) / 8];
    uint8_t *buf;
} rx_slot_t;

struct ilf_herd {
    ilf_herd_io_t io;
    uint8_t  my_mac[6];
    char     my_name[ILF_HERD_NAME_LEN];
    ilf_friend_t friends[ILF_HERD_MAX_FRIENDS];
    int      nfriends;

    /* знакомство */
    bool     bump_active;
    uint32_t bump_ms;
    uint8_t  priv[32], pub[32];
    uint8_t  hellos_left;
    uint32_t next_hello_ms;
    bool     made_friend;
    hello_t  hellos[HELLO_SLOTS];

    /* передача */
    frame_t  ctrl[CTRL_QUEUE];
    int      ctrl_head, ctrl_count;
    bool     tx_busy;
    bool     tx_chunk;          /* в полёте кусок сообщения, а не служебный пакет */
    uint32_t tx_started_ms;

    uint8_t *out_buf;
    size_t   out_len;
    uint8_t  out_kind;
    uint16_t out_id;
    uint16_t out_total, out_idx;
    uint8_t  out_retries;
    uint32_t out_target_ms;     /* когда начали слать этому другу */
    uint32_t out_retry_at;      /* друг спит: следующая попытка не раньше */
    int      out_targets[ILF_HERD_MAX_FRIENDS];
    int      out_ntargets, out_target;
    uint16_t next_msg_id;

    rx_slot_t rx[ILF_HERD_MAX_FRIENDS];
};

/* ---------- помощники ---------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int32_t diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void emit(ilf_herd_t *h, ilf_herd_event_t ev)
{
    if (h->io.event) h->io.event(h->io.ctx, &ev);
}

static void copy_name(char *dst, const char *src)
{
    size_t n = 0;
    while (src && n < ILF_HERD_NAME_LEN - 1 && src[n]) n++;
    /* не резать русскую букву пополам */
    if (src && src[n]) while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

static void header(uint8_t *p, uint8_t type) { p[0] = MAGIC; p[1] = VERSION; p[2] = type; }

static void pump(ilf_herd_t *h, uint32_t now);

static bool enqueue(ilf_herd_t *h, const uint8_t *mac, const uint8_t *data, size_t len, uint32_t now)
{
    if (h->ctrl_count >= CTRL_QUEUE || len > ILF_HERD_MAX_FRAME) return false;
    frame_t *f = &h->ctrl[(h->ctrl_head + h->ctrl_count) % CTRL_QUEUE];
    f->broadcast = mac == NULL;
    if (mac) memcpy(f->mac, mac, 6);
    f->len = (uint8_t)len;
    memcpy(f->data, data, len);
    h->ctrl_count++;
    pump(h, now);
    return true;
}

/* ---------- друзья ---------- */

int ilf_herd_find_friend(const ilf_herd_t *h, const uint8_t mac[6])
{
    for (int i = 0; i < h->nfriends; i++) {
        if (memcmp(h->friends[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

int ilf_herd_friend_count(const ilf_herd_t *h) { return h->nfriends; }

const ilf_friend_t *ilf_herd_friend(const ilf_herd_t *h, int idx)
{
    return idx >= 0 && idx < h->nfriends ? &h->friends[idx] : NULL;
}

void ilf_herd_load_friends(ilf_herd_t *h, const ilf_friend_t *friends, int count)
{
    for (int i = 0; i < count && h->nfriends < ILF_HERD_MAX_FRIENDS; i++) {
        ilf_friend_t *f = &h->friends[h->nfriends++];
        *f = friends[i];
        f->name[ILF_HERD_NAME_LEN - 1] = 0;
        f->seen = false;
        if (h->io.add_peer) h->io.add_peer(h->io.ctx, f->mac, f->key);
    }
}

void ilf_herd_forget_friend(ilf_herd_t *h, int idx)
{
    if (idx < 0 || idx >= h->nfriends) return;
    if (h->io.remove_peer) h->io.remove_peer(h->io.ctx, h->friends[idx].mac);
    free(h->rx[idx].buf);
    for (int i = idx; i < h->nfriends - 1; i++) {
        h->friends[i] = h->friends[i + 1];
        h->rx[i] = h->rx[i + 1];
    }
    h->nfriends--;
    memset(&h->rx[h->nfriends], 0, sizeof h->rx[0]);
    emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_SAVE, .friend_idx = -1 });
}

static int upsert_friend(ilf_herd_t *h, const hello_t *e, uint32_t now)
{
    int idx = ilf_herd_find_friend(h, e->mac);
    if (idx < 0) {
        if (h->nfriends >= ILF_HERD_MAX_FRIENDS) return -1;
        idx = h->nfriends++;
        memset(&h->friends[idx], 0, sizeof h->friends[idx]);
        memset(&h->rx[idx], 0, sizeof h->rx[idx]);
        memcpy(h->friends[idx].mac, e->mac, 6);
    }
    ilf_friend_t *f = &h->friends[idx];
    memcpy(f->key, e->key, 16);
    copy_name(f->name, e->name);
    f->seen = true;
    f->last_seen_ms = now;
    if (h->io.add_peer) h->io.add_peer(h->io.ctx, f->mac, f->key);
    return idx;
}

/* ---------- знакомство ---------- */

/* подтверждение: «я знаю наш общий ключ», привязанное к отправителю */
static void make_proof(ilf_herd_t *h, const uint8_t key[16], const uint8_t sender[6], uint8_t out[8])
{
    uint8_t buf[16 + 3 + 6], dig[32];
    memcpy(buf, key, 16);
    memcpy(buf + 16, "ILF", 3);
    memcpy(buf + 19, sender, 6);
    h->io.sha256(h->io.ctx, buf, sizeof buf, dig);
    memcpy(out, dig, 8);
}

static void send_hello(ilf_herd_t *h, uint32_t now)
{
    uint8_t p[HDR + 32 + 2 + ILF_HERD_NAME_LEN];
    header(p, T_HELLO);
    memcpy(p + HDR, h->pub, 32);
    uint32_t age = now - h->bump_ms;
    put16(p + HDR + 32, (uint16_t)(age > 0xFFFF ? 0xFFFF : age));
    memset(p + HDR + 34, 0, ILF_HERD_NAME_LEN);
    memcpy(p + HDR + 34, h->my_name, strlen(h->my_name));
    enqueue(h, NULL, p, sizeof p, now);
}

static void send_confirm(ilf_herd_t *h, hello_t *e, uint32_t now)
{
    uint8_t p[HDR + 6 + 8];
    header(p, T_CONFIRM);
    memcpy(p + HDR, e->mac, 6);
    make_proof(h, e->key, h->my_mac, p + HDR + 6);
    enqueue(h, NULL, p, sizeof p, now);
}

static void try_match(ilf_herd_t *h, hello_t *e, uint32_t now)
{
    if (!h->bump_active || e->matched) return;
    int32_t d = diff_ms(e->their_bump_ms, h->bump_ms);
    if (d < 0) d = -d;
    if ((uint32_t)d > ILF_HERD_BUMP_MATCH_MS) return;

    uint8_t shared[32];
    if (!h->io.shared(h->io.ctx, h->priv, e->pub, shared)) return;
    /* ключ = SHA-256(общий секрет ‖ меньший pub ‖ больший pub), первые 16 байт */
    uint8_t buf[96], dig[32];
    bool mine_first = memcmp(h->pub, e->pub, 32) < 0;
    memcpy(buf, shared, 32);
    memcpy(buf + 32, mine_first ? h->pub : e->pub, 32);
    memcpy(buf + 64, mine_first ? e->pub : h->pub, 32);
    h->io.sha256(h->io.ctx, buf, sizeof buf, dig);
    memcpy(e->key, dig, 16);
    memset(shared, 0, sizeof shared);
    e->matched = true;
    e->confirms_left = 3;
    send_confirm(h, e, now);
    e->confirms_left--;
}

static hello_t *hello_slot(ilf_herd_t *h, const uint8_t mac[6], uint32_t now)
{
    hello_t *free_slot = NULL, *oldest = &h->hellos[0];
    for (int i = 0; i < HELLO_SLOTS; i++) {
        hello_t *e = &h->hellos[i];
        if (e->used && memcmp(e->mac, mac, 6) == 0) return e;
        if (!e->used && !free_slot) free_slot = e;
        if (diff_ms(e->rx_ms, oldest->rx_ms) < 0) oldest = e;
    }
    hello_t *e = free_slot ? free_slot : oldest;
    memset(e, 0, sizeof *e);
    e->used = true;
    memcpy(e->mac, mac, 6);
    e->rx_ms = now;
    return e;
}

static void on_hello(ilf_herd_t *h, const uint8_t mac[6], const uint8_t *p, size_t len, uint32_t now)
{
    if (len < HDR + 32 + 2 + ILF_HERD_NAME_LEN) return;
    hello_t *e = hello_slot(h, mac, now);
    uint32_t their_bump = now - get16(p + HDR + 32);
    /* новый удар у него — новый ключ, знакомимся заново */
    if (memcmp(e->pub, p + HDR, 32) != 0) {
        memset(e, 0, sizeof *e);
        e->used = true;
        memcpy(e->mac, mac, 6);
        memcpy(e->pub, p + HDR, 32);
    }
    e->rx_ms = now;
    e->their_bump_ms = their_bump;
    char name[ILF_HERD_NAME_LEN];
    memcpy(name, p + HDR + 34, ILF_HERD_NAME_LEN);
    name[ILF_HERD_NAME_LEN - 1] = 0;
    copy_name(e->name, name);
    try_match(h, e, now);
}

static void on_confirm(ilf_herd_t *h, const uint8_t mac[6], const uint8_t *p, size_t len, uint32_t now)
{
    if (len < HDR + 14 || memcmp(p + HDR, h->my_mac, 6) != 0) return;
    for (int i = 0; i < HELLO_SLOTS; i++) {
        hello_t *e = &h->hellos[i];
        if (!e->used || !e->matched || e->confirmed || memcmp(e->mac, mac, 6) != 0) continue;
        uint8_t proof[8];
        make_proof(h, e->key, mac, proof);
        if (memcmp(proof, p + HDR + 6, 8) != 0) return;
        e->confirmed = true;
        int idx = upsert_friend(h, e, now);
        if (idx < 0) return;
        h->made_friend = true;
        emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_FRIEND_ADDED, .friend_idx = idx });
        emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_SAVE, .friend_idx = idx });
        return;
    }
}

void ilf_herd_bump(ilf_herd_t *h, uint32_t now)
{
    if (!h->bump_active) {
        h->io.keypair(h->io.ctx, h->priv, h->pub);
        h->made_friend = false;
    }
    h->bump_active = true;
    h->bump_ms = now;
    h->hellos_left = ILF_HERD_HELLO_COUNT;
    h->next_hello_ms = now;
    /* приветствие второго барашка могло прийти раньше, чем мы заметили удар */
    for (int i = 0; i < HELLO_SLOTS; i++) {
        hello_t *e = &h->hellos[i];
        if (e->used && diff_ms(now, e->rx_ms) < (int32_t)ILF_HERD_BUMP_WINDOW_MS) try_match(h, e, now);
    }
    ilf_herd_tick(h, now);
}

bool ilf_herd_pairing(const ilf_herd_t *h) { return h->bump_active; }

void ilf_herd_set_name(ilf_herd_t *h, const char *name) { copy_name(h->my_name, name); }

static void end_pairing(ilf_herd_t *h)
{
    h->bump_active = false;
    memset(h->priv, 0, sizeof h->priv);
    memset(h->hellos, 0, sizeof h->hellos);
    if (!h->made_friend) emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_PAIR_TIMEOUT, .friend_idx = -1 });
}

/* ---------- сообщения ---------- */

static void finish_target(ilf_herd_t *h, bool ok, uint32_t now)
{
    emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_SENT, .friend_idx = h->out_targets[h->out_target],
                                .msg_id = h->out_id, .ok = ok, .kind = (ilf_msg_kind_t)h->out_kind });
    h->out_target++;
    h->out_idx = 0;
    h->out_retries = 0;
    h->out_target_ms = now;
    h->out_retry_at = now;
    if (h->out_target >= h->out_ntargets) {
        free(h->out_buf);
        h->out_buf = NULL;
    }
}

static void send_chunk(ilf_herd_t *h, uint32_t now)
{
    const ilf_friend_t *f = &h->friends[h->out_targets[h->out_target]];
    uint8_t p[CHUNK_HDR + ILF_HERD_CHUNK_DATA];
    size_t off = (size_t)h->out_idx * ILF_HERD_CHUNK_DATA;
    size_t n = h->out_len - off < ILF_HERD_CHUNK_DATA ? h->out_len - off : ILF_HERD_CHUNK_DATA;
    header(p, T_CHUNK);
    put16(p + HDR, h->out_id);
    put16(p + HDR + 2, h->out_idx);
    put16(p + HDR + 4, h->out_total);
    p[HDR + 6] = h->out_kind;
    memcpy(p + CHUNK_HDR, h->out_buf + off, n);
    h->tx_busy = true;
    h->tx_chunk = true;
    h->tx_started_ms = now;
    h->io.send(h->io.ctx, f->mac, p, CHUNK_HDR + n);
}

static void pump(ilf_herd_t *h, uint32_t now)
{
    if (h->tx_busy) return;
    if (h->ctrl_count) {
        frame_t f = h->ctrl[h->ctrl_head];
        h->ctrl_head = (h->ctrl_head + 1) % CTRL_QUEUE;
        h->ctrl_count--;
        h->tx_busy = true;
        h->tx_chunk = false;
        h->tx_started_ms = now;
        h->io.send(h->io.ctx, f.broadcast ? NULL : f.mac, f.data, f.len);
        return;
    }
    if (h->out_buf && diff_ms(now, h->out_retry_at) >= 0) send_chunk(h, now);
}

void ilf_herd_on_sent(ilf_herd_t *h, bool ok, uint32_t now)
{
    if (!h->tx_busy) return;
    h->tx_busy = false;
    if (h->tx_chunk && h->out_buf) {
        if (ok) {
            h->out_retries = 0;
            if (++h->out_idx >= h->out_total) finish_target(h, true, now);
        } else if (h->out_idx == 0 && diff_ms(now, h->out_target_ms) < (int32_t)ILF_HERD_WAKE_MS) {
            /* друг, наверное, дремлет: радио у него просыпается урывками — стучимся, пока не проснётся */
            h->out_retry_at = now + ILF_HERD_WAKE_RETRY_MS;
        } else if (++h->out_retries > ILF_HERD_CHUNK_RETRIES) {
            finish_target(h, false, now);
        }
    }
    pump(h, now);
}

uint16_t ilf_herd_send(ilf_herd_t *h, int friend_idx, ilf_msg_kind_t kind,
                       const uint8_t *data, size_t len, uint32_t now)
{
    if (h->out_buf || len == 0 || len > ILF_HERD_MAX_MESSAGE) return 0;
    int n = 0;
    if (friend_idx == ILF_HERD_ALL) {
        for (int i = 0; i < h->nfriends; i++) h->out_targets[n++] = i;
    } else if (friend_idx >= 0 && friend_idx < h->nfriends) {
        h->out_targets[n++] = friend_idx;
    }
    if (!n) return 0;
    h->out_buf = malloc(len);
    if (!h->out_buf) return 0;
    memcpy(h->out_buf, data, len);
    h->out_len = len;
    h->out_kind = (uint8_t)kind;
    h->out_total = (uint16_t)((len + ILF_HERD_CHUNK_DATA - 1) / ILF_HERD_CHUNK_DATA);
    h->out_idx = 0;
    h->out_retries = 0;
    h->out_ntargets = n;
    h->out_target = 0;
    h->out_target_ms = now;
    h->out_retry_at = now;
    if (++h->next_msg_id == 0) h->next_msg_id = 1;
    h->out_id = h->next_msg_id;
    pump(h, now);
    return h->out_id;
}

bool ilf_herd_sending(const ilf_herd_t *h) { return h->out_buf != NULL; }

float ilf_herd_send_progress(const ilf_herd_t *h)
{
    if (!h->out_buf || !h->out_ntargets || !h->out_total) return h->out_buf ? 0.f : 1.f;
    float per = 1.f / (float)h->out_ntargets;
    return per * ((float)h->out_target + (float)h->out_idx / (float)h->out_total);
}

static void on_chunk(ilf_herd_t *h, int fi, const uint8_t *p, size_t len, uint32_t now)
{
    if (len < CHUNK_HDR + 1) return;
    uint16_t id = get16(p + HDR), idx = get16(p + HDR + 2), total = get16(p + HDR + 4);
    uint8_t kind = p[HDR + 6];
    size_t n = len - CHUNK_HDR;
    if (!total || total > MAX_CHUNKS || idx >= total || n > ILF_HERD_CHUNK_DATA) return;
    if (idx + 1 < total && n != ILF_HERD_CHUNK_DATA) return;
    rx_slot_t *s = &h->rx[fi];
    if (id == s->last_done_id && !s->active) return;   /* повтор уже доставленного */
    if (!s->active || s->msg_id != id) {
        free(s->buf);
        uint16_t last = s->last_done_id;
        memset(s, 0, sizeof *s);
        s->last_done_id = last;
        s->buf = malloc((size_t)total * ILF_HERD_CHUNK_DATA);
        if (!s->buf) return;
        s->active = true;
        s->msg_id = id;
        s->total = total;
        s->kind = kind;
    }
    s->last_rx_ms = now;
    if (s->bitmap[idx / 8] & (1u << (idx % 8))) return;
    s->bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
    memcpy(s->buf + (size_t)idx * ILF_HERD_CHUNK_DATA, p + CHUNK_HDR, n);
    if (idx + 1 == total) s->len = (size_t)idx * ILF_HERD_CHUNK_DATA + n;
    if (++s->count == s->total) {
        emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_MESSAGE, .friend_idx = fi, .msg_id = id,
                                    .kind = (ilf_msg_kind_t)s->kind, .data = s->buf, .len = s->len, .ok = true });
        free(s->buf);
        s->buf = NULL;
        s->active = false;
        s->last_done_id = id;
    }
}

/* ---------- рисунки ---------- */

void ilf_herd_send_stroke(ilf_herd_t *h, int friend_idx, const ilf_stroke_t *s, uint32_t now)
{
    uint8_t p[HDR + 3 + ILF_HERD_STROKE_POINTS * 4];
    uint8_t n = s->count > ILF_HERD_STROKE_POINTS ? ILF_HERD_STROKE_POINTS : s->count;
    header(p, T_STROKE);
    p[HDR] = s->stroke_id;
    p[HDR + 1] = s->clear ? 1 : 0;
    p[HDR + 2] = n;
    for (int i = 0; i < n; i++) {
        put16(p + HDR + 3 + i * 4, s->pts[i].x);
        put16(p + HDR + 5 + i * 4, s->pts[i].y);
    }
    size_t len = HDR + 3 + (size_t)n * 4;
    for (int i = 0; i < h->nfriends; i++) {
        if (friend_idx == ILF_HERD_ALL || friend_idx == i) enqueue(h, h->friends[i].mac, p, len, now);
    }
}

bool ilf_herd_send_live(ilf_herd_t *h, int friend_idx, uint16_t seq, const uint8_t *data, size_t len, uint32_t now)
{
    if (len > ILF_HERD_CHUNK_DATA) return false;
    uint8_t p[HDR + 2 + ILF_HERD_CHUNK_DATA];
    header(p, T_LIVE);
    put16(p + HDR, seq);
    if (len) memcpy(p + HDR + 2, data, len);
    bool ok = true;
    for (int i = 0; i < h->nfriends; i++) {
        if (friend_idx != ILF_HERD_ALL && friend_idx != i) continue;
        /* очередь почти полна — живой звук уступает служебным пакетам */
        if (h->ctrl_count >= CTRL_QUEUE - 2 || !enqueue(h, h->friends[i].mac, p, HDR + 2 + len, now)) ok = false;
    }
    return ok;
}

static void on_live(ilf_herd_t *h, int fi, const uint8_t *p, size_t len)
{
    if (len < HDR + 2 || len > HDR + 2 + ILF_HERD_CHUNK_DATA) return;
    emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_LIVE, .friend_idx = fi, .msg_id = get16(p + HDR),
                                .data = p + HDR + 2, .len = len - HDR - 2 });
}

static void on_stroke(ilf_herd_t *h, int fi, const uint8_t *p, size_t len)
{
    if (len < HDR + 3) return;
    ilf_stroke_t s = { .stroke_id = p[HDR], .clear = p[HDR + 1] & 1, .count = p[HDR + 2] };
    if (s.count > ILF_HERD_STROKE_POINTS || len < HDR + 3 + (size_t)s.count * 4) return;
    for (int i = 0; i < s.count; i++) {
        s.pts[i].x = get16(p + HDR + 3 + i * 4);
        s.pts[i].y = get16(p + HDR + 5 + i * 4);
    }
    emit(h, (ilf_herd_event_t){ .type = ILF_HERD_EV_STROKE, .friend_idx = fi, .stroke = &s });
}

/* ---------- приём и таймеры ---------- */

void ilf_herd_on_recv(ilf_herd_t *h, const uint8_t mac[6], const uint8_t *data, size_t len, uint32_t now)
{
    if (len < HDR || data[0] != MAGIC || data[1] != VERSION) return;
    if (memcmp(mac, h->my_mac, 6) == 0) return;
    switch (data[2]) {
    case T_HELLO:   on_hello(h, mac, data, len, now); return;
    case T_CONFIRM: on_confirm(h, mac, data, len, now); return;
    default: break;
    }
    /* остальное — только от друзей */
    int fi = ilf_herd_find_friend(h, mac);
    if (fi < 0) return;
    h->friends[fi].seen = true;
    h->friends[fi].last_seen_ms = now;
    if (data[2] == T_CHUNK) on_chunk(h, fi, data, len, now);
    else if (data[2] == T_STROKE) on_stroke(h, fi, data, len);
    else if (data[2] == T_LIVE) on_live(h, fi, data, len);
}

void ilf_herd_tick(ilf_herd_t *h, uint32_t now)
{
    if (h->bump_active) {
        if (h->hellos_left && diff_ms(now, h->next_hello_ms) >= 0) {
            h->hellos_left--;
            h->next_hello_ms = now + ILF_HERD_HELLO_REPEAT_MS;
            send_hello(h, now);
            for (int i = 0; i < HELLO_SLOTS; i++) {
                hello_t *e = &h->hellos[i];
                if (e->used && e->matched && e->confirms_left) {
                    e->confirms_left--;
                    send_confirm(h, e, now);
                }
            }
        }
        if (diff_ms(now, h->bump_ms) > (int32_t)ILF_HERD_BUMP_WINDOW_MS) end_pairing(h);
    }
    for (int i = 0; i < h->nfriends; i++) {
        rx_slot_t *s = &h->rx[i];
        if (s->active && diff_ms(now, s->last_rx_ms) > (int32_t)ILF_HERD_RX_TIMEOUT_MS) {
            free(s->buf);
            s->buf = NULL;
            s->active = false;
        }
    }
    /* радио не ответило про отправку — не висим вечно */
    if (h->tx_busy && diff_ms(now, h->tx_started_ms) > (int32_t)TX_STUCK_MS) ilf_herd_on_sent(h, false, now);
    pump(h, now);
}

/* ---------- создание ---------- */

ilf_herd_t *ilf_herd_create(const ilf_herd_io_t *io, const uint8_t my_mac[6], const char *my_name)
{
    ilf_herd_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->io = *io;
    memcpy(h->my_mac, my_mac, 6);
    copy_name(h->my_name, my_name);
    h->next_msg_id = (uint16_t)(my_mac[4] << 8 | my_mac[5]);
    return h;
}

void ilf_herd_destroy(ilf_herd_t *h)
{
    if (!h) return;
    free(h->out_buf);
    for (int i = 0; i < ILF_HERD_MAX_FRIENDS; i++) free(h->rx[i].buf);
    free(h);
}
