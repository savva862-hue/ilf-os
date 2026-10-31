#include "ilf_ota.h"

#include <string.h>

enum { T_VER = 1, T_REQ = 2, T_DATA = 3 };

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void emit(ilf_ota_t *o, ilf_ota_ev_type_t t, float progress)
{
    if (!o->io.event) return;
    ilf_ota_event_t ev = { .type = t, .version = o->want.version, .progress = progress };
    memcpy(ev.mac, o->src, 6);
    o->io.event(o->io.user, &ev);
}

void ilf_ota_init(ilf_ota_t *o, const ilf_ota_io_t *io, const ilf_ota_image_t *mine)
{
    memset(o, 0, sizeof *o);
    o->io = *io;
    o->mine = *mine;
}

static void send_ver(ilf_ota_t *o, const uint8_t mac[6])
{
    uint8_t f[2 + 2 + 4 + 32];
    f[0] = ILF_OTA_MAGIC; f[1] = T_VER;
    put16(f + 2, o->mine.version);
    put32(f + 4, o->mine.size);
    memcpy(f + 8, o->mine.sha, 32);
    o->io.send(o->io.user, mac, f, sizeof f);
}

void ilf_ota_seen(ilf_ota_t *o, const uint8_t mac[6], uint32_t now)
{
    if (!o->mine.size) return;   /* свою прошивку прочитать не смогли — раздавать нечего */
    int slot = -1, oldest = 0;
    for (int i = 0; i < ILF_OTA_PEERS; i++) {
        if (o->told[i].used && memcmp(o->told[i].mac, mac, 6) == 0) { slot = i; break; }
        if (!o->told[i].used) { if (slot < 0) slot = i; continue; }
        if ((int32_t)(o->told[i].at - o->told[oldest].at) < 0) oldest = i;
    }
    if (slot < 0) slot = oldest;
    bool known = o->told[slot].used && memcmp(o->told[slot].mac, mac, 6) == 0;
    if (known && now - o->told[slot].at < ILF_OTA_ANNOUNCE_MS) return;
    o->told[slot].used = true;
    memcpy(o->told[slot].mac, mac, 6);
    o->told[slot].at = now;
    send_ver(o, mac);
}

static void request(ilf_ota_t *o, uint32_t now)
{
    uint32_t left = (o->want.size - o->next + ILF_OTA_CHUNK - 1) / ILF_OTA_CHUNK;
    uint8_t count = (uint8_t)(left < ILF_OTA_WINDOW ? left : ILF_OTA_WINDOW);
    uint8_t f[7] = { ILF_OTA_MAGIC, T_REQ };
    put32(f + 2, o->next);
    f[6] = count;
    o->window_end = o->next + (uint32_t)count * ILF_OTA_CHUNK;
    if (o->window_end > o->want.size) o->window_end = o->want.size;
    o->last_rx_ms = now;
    o->io.send(o->io.user, o->src, f, sizeof f);
}

static void fail(ilf_ota_t *o)
{
    if (o->io.abort) o->io.abort(o->io.user);
    o->state = ILF_OTA_IDLE;
    emit(o, ILF_OTA_EV_FAILED, 0);
}

void ilf_ota_accept(ilf_ota_t *o, uint32_t now)
{
    if (o->state != ILF_OTA_OFFERED) return;
    if (!o->io.begin(o->io.user, o->want.size)) { fail(o); return; }
    o->state = ILF_OTA_PULLING;
    o->next = 0;
    o->retries = 0;
    o->last_progress = 0;
    emit(o, ILF_OTA_EV_PROGRESS, 0);
    request(o, now);
}

void ilf_ota_decline(ilf_ota_t *o, bool forever)
{
    if (o->state != ILF_OTA_OFFERED) return;
    if (forever) o->declined = o->want.version;
    o->state = ILF_OTA_IDLE;
}

static void on_ver(ilf_ota_t *o, const uint8_t mac[6], const uint8_t *d, size_t n, uint32_t now)
{
    if (n < 40) return;
    ilf_ota_image_t img = { .version = get16(d + 2), .size = get32(d + 4) };
    memcpy(img.sha, d + 8, 32);
    /* друг не знает нашей версии — ответим, вдруг у нас новее */
    ilf_ota_seen(o, mac, now);
    if (o->state != ILF_OTA_IDLE) return;
    if (img.version <= o->mine.version || img.version == o->declined) return;
    if (img.size == 0 || img.size > 16u * 1024u * 1024u) return;
    o->want = img;
    memcpy(o->src, mac, 6);
    o->state = ILF_OTA_OFFERED;
    emit(o, ILF_OTA_EV_OFFER, 0);
}

static void on_req(ilf_ota_t *o, const uint8_t mac[6], const uint8_t *d, size_t n)
{
    if (n < 7 || !o->mine.size) return;
    uint32_t off = get32(d + 2);
    if (off >= o->mine.size || off % ILF_OTA_CHUNK) return;
    /* раздаём одному: другой друг подождёт и переспросит */
    if (o->serving && memcmp(o->dst, mac, 6) != 0) return;
    uint32_t end = off + (uint32_t)d[6] * ILF_OTA_CHUNK;
    if (end > o->mine.size) end = o->mine.size;
    o->serving = end > off;
    memcpy(o->dst, mac, 6);
    o->serve_off = off;
    o->serve_end = end;
}

static void on_data(ilf_ota_t *o, const uint8_t mac[6], const uint8_t *d, size_t n, uint32_t now)
{
    if (o->state != ILF_OTA_PULLING || memcmp(mac, o->src, 6) != 0 || n <= 6) return;
    uint32_t off = get32(d + 2);
    size_t len = n - 6;
    if (off != o->next) return;                     /* кусок потерялся раньше — дождёмся переспроса */
    if (off + len > o->want.size) return;
    if (!o->io.write(o->io.user, d + 6, len)) { fail(o); return; }
    o->next += (uint32_t)len;
    o->last_rx_ms = now;
    o->retries = 0;
    float p = (float)o->next / (float)o->want.size;
    if (p - o->last_progress >= 0.01f || o->next == o->want.size) {
        o->last_progress = p;
        emit(o, ILF_OTA_EV_PROGRESS, p);
    }
    if (o->next == o->want.size) {
        if (o->io.finish(o->io.user, o->want.sha)) {
            o->state = ILF_OTA_READY;
            emit(o, ILF_OTA_EV_DONE, 1);
        } else {
            o->state = ILF_OTA_IDLE;
            emit(o, ILF_OTA_EV_FAILED, 0);
        }
        return;
    }
    if (o->next >= o->window_end) request(o, now);
}

void ilf_ota_on_recv(ilf_ota_t *o, const uint8_t mac[6], const uint8_t *d, size_t n, uint32_t now)
{
    if (n < 2 || d[0] != ILF_OTA_MAGIC) return;
    switch (d[1]) {
    case T_VER:  on_ver(o, mac, d, n, now); break;
    case T_REQ:  on_req(o, mac, d, n); break;
    case T_DATA: on_data(o, mac, d, n, now); break;
    default: break;
    }
}

void ilf_ota_tick(ilf_ota_t *o, uint32_t now)
{
    /* раздача: несколько кусков за тик, чтобы не забить очередь радио */
    for (int i = 0; o->serving && i < ILF_OTA_SEND_PER_TICK; i++) {
        uint8_t f[ILF_OTA_MAX_FRAME] = { ILF_OTA_MAGIC, T_DATA };
        uint32_t len = o->serve_end - o->serve_off;
        if (len > ILF_OTA_CHUNK) len = ILF_OTA_CHUNK;
        put32(f + 2, o->serve_off);
        if (!o->io.read(o->io.user, o->serve_off, f + 6, len)) { o->serving = false; break; }
        if (!o->io.send(o->io.user, o->dst, f, 6 + len)) break;
        o->serve_off += len;
        if (o->serve_off >= o->serve_end) o->serving = false;
    }
    /* приём: тишина — переспросить с того места, где остановились */
    if (o->state == ILF_OTA_PULLING && now - o->last_rx_ms > ILF_OTA_RETRY_MS) {
        if (++o->retries > ILF_OTA_MAX_RETRIES) { fail(o); return; }
        request(o, now);
    }
}

bool ilf_ota_busy(const ilf_ota_t *o) { return o->serving || o->state == ILF_OTA_PULLING; }
