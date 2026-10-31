#include "ilf_voice.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ilf_board.h"
#include "ilf_herd.h"

static const char *TAG = "ilf_voice";

#define SD_MOUNT  "/sd"
#define SD_DIR    "/sd/ilf"
#define FLASH_DIR "/store"
#define MAGIC1    0x56464C49u   /* "ILFV" — v0.3, без имени */
#define MAGIC2    0x32464C49u   /* "ILF2" — с именем и флагами */
#define F_UPLOADED 0x01
#define F_MINE     0x02
#define LIVE_MAX_BLOCKS (ILF_VOICE_RATE * 300 / ILF_ADPCM_BLOCK_SAMPLES)   /* рация: до 5 минут подряд */
#define FLASH_RESERVE   (96 * 1024)   /* во внутренней памяти всегда оставляем свободным */

typedef struct {
    uint32_t magic;
    uint8_t  mac[6];
    uint8_t  played;
    uint8_t  flags;
    uint32_t len;
    uint32_t time;
    char     name[ILF_HERD_NAME_LEN];   /* только в MAGIC2 */
} file_hdr_t;
#define HDR1_SIZE 20u

typedef struct {
    uint32_t seq;
    bool     played;
    uint8_t  flags;
    uint32_t len;
    uint32_t time;
    uint32_t off;      /* где начинается звук */
    char     name[ILF_HERD_NAME_LEN];
} entry_t;

static const char *s_dir = FLASH_DIR;
static bool s_sd, s_fs_ok;
static SemaphoreHandle_t s_mx;
static entry_t s_items[ILF_VOICE_KEEP];   /* входящие по возрастанию seq: последний — самый новый */
static int s_count;
static uint32_t s_next_seq = 1;

/* ---------- задача звука ---------- */

typedef enum { CMD_REC_START, CMD_REC_STOP, CMD_PLAY } cmd_type_t;
typedef struct { cmd_type_t type; uint8_t *data; size_t len; bool live; } cmd_t;
typedef struct { uint8_t len; uint8_t data[ILF_ADPCM_BLOCK_BYTES]; } live_blk_t;

static QueueHandle_t s_cmd, s_live_out, s_live_in;
static volatile bool s_recording, s_playing, s_hearing;
static volatile size_t s_rec_blocks;
static uint8_t *volatile s_done;
static volatile size_t s_done_len;

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

static void record(bool live)
{
    uint8_t *buf = live ? NULL : big_alloc(ILF_HERD_MAX_MESSAGE);
    if (!live && !buf) { s_recording = false; return; }
    int16_t pcm[ILF_ADPCM_BLOCK_SAMPLES];
    uint8_t blk[ILF_ADPCM_BLOCK_BYTES];
    ilf_adpcm_state_t st = { 0 };
    size_t max_blocks = live ? LIVE_MAX_BLOCKS : ILF_HERD_MAX_MESSAGE / ILF_ADPCM_BLOCK_BYTES;
    s_rec_blocks = 0;
    /* первые 60 мс — щелчок кнопки и раскачка микрофона, их выбрасываем */
    ilf_audio_read(pcm, ILF_VOICE_RATE * 60 / 1000, 100);
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmd, &c, 0) == pdTRUE && c.type == CMD_REC_STOP) break;
        if (s_rec_blocks >= max_blocks) break;   /* минута для голосового, 5 минут для рации */
        size_t got = ilf_audio_read(pcm, ILF_ADPCM_BLOCK_SAMPLES, 200);
        if (got < ILF_ADPCM_BLOCK_SAMPLES) memset(pcm + got, 0, (ILF_ADPCM_BLOCK_SAMPLES - got) * sizeof(int16_t));
        if (live) {
            ilf_adpcm_encode_block(&st, pcm, blk);
            xQueueSend(s_live_out, blk, 0);   /* эфир не успевает — блок теряется, это лучше задержки */
        } else {
            ilf_adpcm_encode_block(&st, pcm, buf + s_rec_blocks * ILF_ADPCM_BLOCK_BYTES);
        }
        s_rec_blocks++;
    }
    s_recording = false;
    if (live) return;
    if (s_rec_blocks < 2) { free(buf); return; }   /* случайное касание */
    s_done_len = s_rec_blocks * ILF_ADPCM_BLOCK_BYTES;
    s_done = buf;
}

static void play(uint8_t *data, size_t len)
{
    int16_t pcm[ILF_ADPCM_BLOCK_SAMPLES];
    ilf_audio_speaker(true);
    for (size_t off = 0; off + ILF_ADPCM_BLOCK_BYTES <= len; off += ILF_ADPCM_BLOCK_BYTES) {
        ilf_adpcm_decode_block(data + off, pcm);
        ilf_audio_write(pcm, ILF_ADPCM_BLOCK_SAMPLES, 500);
    }
    memset(pcm, 0, sizeof pcm);
    ilf_audio_write(pcm, ILF_ADPCM_BLOCK_SAMPLES, 500);   /* дослушать хвост из DMA */
    ilf_audio_speaker(false);
    free(data);
    s_playing = false;
}

/* Рация: копим ~170 мс, чтобы неровный эфир не рвал звук, и играем, пока идут блоки. */
static void play_live(void)
{
    for (int i = 0; i < 6 && uxQueueMessagesWaiting(s_live_in) < 3; i++) vTaskDelay(pdMS_TO_TICKS(20));
    int16_t pcm[ILF_ADPCM_BLOCK_SAMPLES];
    live_blk_t b;
    s_hearing = true;
    ilf_audio_speaker(true);
    while (!uxQueueMessagesWaiting(s_cmd) && xQueueReceive(s_live_in, &b, pdMS_TO_TICKS(400)) == pdTRUE) {
        if (b.len != ILF_ADPCM_BLOCK_BYTES) continue;
        ilf_adpcm_decode_block(b.data, pcm);
        ilf_audio_write(pcm, ILF_ADPCM_BLOCK_SAMPLES, 500);
    }
    memset(pcm, 0, sizeof pcm);
    ilf_audio_write(pcm, ILF_ADPCM_BLOCK_SAMPLES, 500);
    ilf_audio_speaker(false);
    s_hearing = false;
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmd, &c, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (c.type == CMD_REC_STOP) continue;
            ilf_audio_begin();
            if (c.type == CMD_REC_START) record(c.live);
            else if (c.type == CMD_PLAY) play(c.data, c.len);
            ilf_audio_end();
        } else if (uxQueueMessagesWaiting(s_live_in)) {
            ilf_audio_begin();
            play_live();
            ilf_audio_end();
        }
    }
}

void ilf_voice_record_start(bool live)
{
    if (s_recording || s_playing || !ilf_audio_ok()) return;
    s_recording = true;
    s_rec_blocks = 0;
    xQueueReset(s_live_out);
    cmd_t c = { .type = CMD_REC_START, .live = live };
    xQueueSend(s_cmd, &c, 0);
}

void ilf_voice_record_stop(void)
{
    if (!s_recording) return;
    cmd_t c = { .type = CMD_REC_STOP };
    xQueueSend(s_cmd, &c, 0);
}

bool ilf_voice_recording(void) { return s_recording; }

float ilf_voice_recorded_seconds(void)
{
    return (float)(s_rec_blocks * ILF_ADPCM_BLOCK_SAMPLES) / (float)ILF_VOICE_RATE;
}

bool ilf_voice_take_recording(uint8_t **data, size_t *len)
{
    if (!s_done) return false;
    *data = s_done;
    *len = s_done_len;
    s_done = NULL;
    return true;
}

bool ilf_voice_playing(void) { return s_playing; }

bool ilf_voice_live_take(uint8_t block[ILF_ADPCM_BLOCK_BYTES])
{
    return s_live_out && xQueueReceive(s_live_out, block, 0) == pdTRUE;
}

void ilf_voice_live_feed(const uint8_t *block, size_t len)
{
    /* сами говорим — друга не слышим, иначе динамик зазвенит в микрофон */
    if (!s_live_in || s_recording || s_playing || len != ILF_ADPCM_BLOCK_BYTES) return;
    live_blk_t b = { .len = (uint8_t)len };
    memcpy(b.data, block, len);
    if (xQueueSend(s_live_in, &b, 0) != pdTRUE) {
        live_blk_t old;
        xQueueReceive(s_live_in, &old, 0);   /* отстаём — выкидываем самый старый */
        xQueueSend(s_live_in, &b, 0);
    }
}

bool ilf_voice_live_hearing(void) { return s_hearing || (s_live_in && uxQueueMessagesWaiting(s_live_in)); }

/* ---------- хранилище ---------- */

static void lock(void) { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

static void path_of(char kind, uint32_t seq, char *out, size_t n)
{
    snprintf(out, n, "%s/%c%05lu.%s", s_dir, kind, (unsigned long)seq, kind == 'p' ? "bmp" : "ilf");
}

static bool read_hdr(FILE *f, file_hdr_t *h, uint32_t *off)
{
    memset(h, 0, sizeof *h);
    if (fread(h, HDR1_SIZE, 1, f) != 1) return false;
    if (h->magic == MAGIC1) { *off = HDR1_SIZE; return true; }
    if (h->magic != MAGIC2) return false;
    if (fread(h->name, sizeof h->name, 1, f) != 1) return false;
    h->name[sizeof h->name - 1] = 0;
    *off = sizeof *h;
    return true;
}

static int cmp_seq(const void *a, const void *b)
{
    uint32_t x = ((const entry_t *)a)->seq, y = ((const entry_t *)b)->seq;
    return x < y ? -1 : x > y;
}

static size_t free_bytes(void)
{
    if (s_sd) {
        uint64_t total = 0, fr = 0;
        if (esp_vfs_fat_info(SD_MOUNT, &total, &fr) == ESP_OK) return fr > SIZE_MAX ? SIZE_MAX : (size_t)fr;
        return SIZE_MAX;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK || used > total) return 0;
    return total - used;
}

/* Перечитать папку: входящие — в список, лишнее — удалить. Под замком. */
static void scan_locked(void)
{
    DIR *d = opendir(s_dir);
    if (!d) return;
    static entry_t all[ILF_VOICE_KEEP * 3];
    int n = 0;
    uint32_t max_seq = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        unsigned long seq;
        char kind = e->d_name[0];
        if ((kind != 'v' && kind != 'm' && kind != 'p') || sscanf(e->d_name + 1, "%5lu", &seq) != 1) continue;
        if (seq > max_seq) max_seq = (uint32_t)seq;
        if (kind != 'v') continue;
        char p[64];
        path_of('v', (uint32_t)seq, p, sizeof p);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        file_hdr_t h;
        uint32_t off;
        bool ok = read_hdr(f, &h, &off);
        fclose(f);
        if (!ok) { remove(p); continue; }
        if (n == (int)(sizeof all / sizeof all[0])) {
            /* очень много файлов: самый старый из набранных уходит */
            qsort(all, (size_t)n, sizeof all[0], cmp_seq);
            char op[64];
            path_of('v', all[0].seq, op, sizeof op);
            remove(op);
            memmove(all, all + 1, sizeof all[0] * (size_t)--n);
        }
        all[n] = (entry_t){ .seq = (uint32_t)seq, .played = h.played, .flags = h.flags, .len = h.len,
                            .time = h.time, .off = off };
        memcpy(all[n].name, h.name, sizeof all[n].name);
        n++;
    }
    closedir(d);
    qsort(all, (size_t)n, sizeof all[0], cmp_seq);
    int start = n > ILF_VOICE_KEEP ? n - ILF_VOICE_KEEP : 0;
    for (int i = 0; i < start; i++) {
        char p[64];
        path_of('v', all[i].seq, p, sizeof p);
        remove(p);
    }
    s_count = n - start;
    memcpy(s_items, all + start, sizeof(entry_t) * (size_t)s_count);
    if (max_seq >= s_next_seq) s_next_seq = max_seq + 1;
    if (s_next_seq > 99999) s_next_seq = 1;
}

/* Освободить место: сначала то, что уже на Диске, потом самое старое. Под замком. */
static void make_room_locked(size_t need)
{
    for (int guard = 0; guard < 64 && free_bytes() < need + (s_sd ? 0 : FLASH_RESERVE); guard++) {
        DIR *d = opendir(s_dir);
        if (!d) return;
        char victim[64] = "";
        uint32_t best = UINT32_MAX;
        bool best_up = false;
        struct dirent *e;
        while ((e = readdir(d))) {
            unsigned long seq;
            char kind = e->d_name[0];
            if ((kind != 'v' && kind != 'm' && kind != 'p') || sscanf(e->d_name + 1, "%5lu", &seq) != 1) continue;
            char p[64];
            path_of(kind, (uint32_t)seq, p, sizeof p);
            bool up = false;
            if (kind != 'p') {
                FILE *f = fopen(p, "rb");
                file_hdr_t h;
                uint32_t off;
                if (f && read_hdr(f, &h, &off)) up = h.flags & F_UPLOADED;
                if (f) fclose(f);
            }
            if ((up && !best_up) || (up == best_up && seq < best)) {
                best = (uint32_t)seq;
                best_up = up;
                snprintf(victim, sizeof victim, "%s", p);
            }
        }
        closedir(d);
        if (!victim[0]) return;
        ESP_LOGW(TAG, "место кончается, удаляю %s", victim);
        remove(victim);
    }
}

static uint32_t take_seq(void)
{
    uint32_t seq = s_next_seq++;
    if (s_next_seq > 99999) s_next_seq = 1;
    return seq;
}

void ilf_voice_store(const uint8_t mac[6], const char *name, const uint8_t *data, size_t len, bool mine)
{
    if (!s_fs_ok) return;
    lock();
    make_room_locked(len + sizeof(file_hdr_t));
    uint32_t seq = take_seq();
    char p[64];
    path_of(mine ? 'm' : 'v', seq, p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) { unlock(); ESP_LOGW(TAG, "не записать %s", p); return; }
    file_hdr_t h = { .magic = MAGIC2, .len = (uint32_t)len, .time = (uint32_t)time(NULL),
                     .flags = mine ? F_MINE : 0, .played = mine };
    memcpy(h.mac, mac, 6);
    snprintf(h.name, sizeof h.name, "%s", name ? name : "");
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(data, 1, len, f) == len;
    fclose(f);
    if (!ok) {
        remove(p);
        ESP_LOGW(TAG, "место кончилось");
    } else if (!mine) {
        if (s_count == ILF_VOICE_KEEP) {
            char op[64];
            path_of('v', s_items[0].seq, op, sizeof op);
            remove(op);
            memmove(s_items, s_items + 1, sizeof(entry_t) * (ILF_VOICE_KEEP - 1));
            s_count--;
        }
        entry_t *e = &s_items[s_count++];
        *e = (entry_t){ .seq = seq, .len = (uint32_t)len, .time = h.time, .off = sizeof h };
        memcpy(e->name, h.name, sizeof e->name);
    }
    unlock();
}

int ilf_voice_items(ilf_ui_voice_item_t *out, int max)
{
    lock();
    int n = 0;
    for (int i = s_count - 1; i >= 0 && n < max; i--, n++) {
        const entry_t *e = &s_items[i];
        snprintf(out[n].from, sizeof out[n].from, "%s", e->name[0] ? e->name : "Барашек");
        out[n].seconds = (uint16_t)((e->len / ILF_ADPCM_BLOCK_BYTES) * ILF_ADPCM_BLOCK_SAMPLES / ILF_VOICE_RATE);
        out[n].unplayed = !e->played;
    }
    unlock();
    return n;
}

bool ilf_voice_play(int item)
{
    if (s_playing || s_recording || !ilf_audio_ok()) return false;
    lock();
    int i = s_count - 1 - item;
    if (i < 0) { unlock(); return false; }
    entry_t *e = &s_items[i];
    char p[64];
    path_of('v', e->seq, p, sizeof p);
    FILE *f = fopen(p, "r+b");
    uint8_t *buf = f ? big_alloc(e->len) : NULL;
    file_hdr_t h;
    uint32_t off = 0;
    bool ok = buf && read_hdr(f, &h, &off) && fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, e->len, f) == e->len;
    if (ok && !h.played) {
        h.played = 1;
        fseek(f, 0, SEEK_SET);
        fwrite(&h, HDR1_SIZE, 1, f);
    }
    if (f) fclose(f);
    if (ok) e->played = true;
    unlock();
    if (!ok) { free(buf); return false; }
    s_playing = true;
    cmd_t c = { .type = CMD_PLAY, .data = buf, .len = e->len };
    xQueueSend(s_cmd, &c, 0);
    return true;
}

/* ---------- рисунки ---------- */

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

void ilf_voice_save_drawing(const uint16_t *px, int w, int h)
{
    if (!s_fs_ok || w <= 0 || h <= 0) return;
    /* BMP 16 бит RGB565 (BI_BITFIELDS), строки снизу вверх */
    uint32_t row = (uint32_t)w * 2, pad = (4 - row % 4) % 4, data = (row + pad) * (uint32_t)h;
    uint8_t hdr[66] = { 'B', 'M' };
    le32(hdr + 2, sizeof hdr + data);
    le32(hdr + 10, sizeof hdr);
    le32(hdr + 14, 40);
    le32(hdr + 18, (uint32_t)w);
    le32(hdr + 22, (uint32_t)h);
    le16(hdr + 26, 1);
    le16(hdr + 28, 16);
    le32(hdr + 30, 3);
    le32(hdr + 34, data);
    le32(hdr + 54, 0xF800);
    le32(hdr + 58, 0x07E0);
    le32(hdr + 62, 0x001F);
    lock();
    make_room_locked(sizeof hdr + data);
    char p[64];
    path_of('p', take_seq(), p, sizeof p);
    FILE *f = fopen(p, "wb");
    bool ok = f && fwrite(hdr, sizeof hdr, 1, f) == 1;
    static const uint8_t zero[4] = { 0 };
    for (int y = h - 1; ok && y >= 0; y--) {
        ok = fwrite(px + (size_t)y * (size_t)w, 2, (size_t)w, f) == (size_t)w && (!pad || fwrite(zero, 1, pad, f) == pad);
    }
    if (f) fclose(f);
    if (!ok) remove(p);
    else ESP_LOGI(TAG, "рисунок сохранён: %s", p);
    unlock();
}

/* ---------- выгрузка на Диск ---------- */

static void stamp(uint32_t t, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm lt;
    if (t > 1600000000u && localtime_r(&tt, &lt)) strftime(out, n, "%Y-%m-%d %H-%M-%S", &lt);
    else snprintf(out, n, "без даты");
}

static size_t wav_size(uint32_t adpcm_len)
{
    return 44 + (size_t)(adpcm_len / ILF_ADPCM_BLOCK_BYTES) * ILF_ADPCM_BLOCK_SAMPLES * 2;
}

bool ilf_voice_next_upload(ilf_upload_t *u)
{
    if (!s_fs_ok) return false;
    lock();
    DIR *d = opendir(s_dir);
    bool found = false;
    uint32_t best = UINT32_MAX;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        unsigned long seq;
        char kind = e->d_name[0];
        if ((kind != 'v' && kind != 'm' && kind != 'p') || sscanf(e->d_name + 1, "%5lu", &seq) != 1) continue;
        if ((uint32_t)seq >= best) continue;
        char p[48];
        path_of(kind, (uint32_t)seq, p, sizeof p);
        char when[32];
        if (kind == 'p') {
            struct stat st;
            if (stat(p, &st) != 0) continue;
            stamp((uint32_t)st.st_mtime, when, sizeof when);
            snprintf(u->name, sizeof u->name, "Рисунок %s %05lu.bmp", when, seq);
            u->size = (size_t)st.st_size;
            u->voice = false;
        } else {
            FILE *f = fopen(p, "rb");
            file_hdr_t h;
            uint32_t off;
            bool ok = f && read_hdr(f, &h, &off);
            if (f) fclose(f);
            if (!ok || (h.flags & F_UPLOADED)) continue;
            stamp(h.time, when, sizeof when);
            if (kind == 'm') snprintf(u->name, sizeof u->name, "%s я %05lu.wav", when, seq);
            else snprintf(u->name, sizeof u->name, "%s %s %05lu.wav", when, h.name[0] ? h.name : "друг", seq);
            u->size = wav_size(h.len);
            u->voice = true;
        }
        snprintf(u->path, sizeof u->path, "%s", p);
        u->seq = (uint32_t)seq;
        best = (uint32_t)seq;
        found = true;
    }
    if (d) closedir(d);
    unlock();
    return found;
}

int ilf_voice_pending_uploads(void)
{
    if (!s_fs_ok) return 0;
    int n = 0;
    lock();
    DIR *d = opendir(s_dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char kind = e->d_name[0];
        if (kind == 'p') { n++; continue; }
        if (kind != 'v' && kind != 'm') continue;
        unsigned long seq;
        if (sscanf(e->d_name + 1, "%5lu", &seq) != 1) continue;
        char p[64];
        path_of(kind, (uint32_t)seq, p, sizeof p);
        FILE *f = fopen(p, "rb");
        file_hdr_t h;
        uint32_t off;
        if (f && read_hdr(f, &h, &off) && !(h.flags & F_UPLOADED)) n++;
        if (f) fclose(f);
    }
    if (d) closedir(d);
    unlock();
    return n;
}

bool ilf_voice_read_upload(const ilf_upload_t *u, ilf_voice_sink_fn sink, void *user)
{
    /* файл читаем без замка: удалить его может только ilf_voice_uploaded, а её зовём после */
    FILE *f = fopen(u->path, "rb");
    if (!f) return false;
    bool ok = true;
    if (!u->voice) {
        uint8_t buf[1024];
        size_t n;
        while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) ok = sink(buf, n, user);
        fclose(f);
        return ok;
    }
    file_hdr_t h;
    uint32_t off;
    if (!read_hdr(f, &h, &off) || fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return false; }
    uint32_t pcm_bytes = (uint32_t)(wav_size(h.len) - 44);
    uint8_t w[44] = { 'R', 'I', 'F', 'F' };
    le32(w + 4, 36 + pcm_bytes);
    memcpy(w + 8, "WAVEfmt ", 8);
    le32(w + 16, 16);
    le16(w + 20, 1);                       /* PCM */
    le16(w + 22, 1);                       /* моно */
    le32(w + 24, ILF_VOICE_RATE);
    le32(w + 28, ILF_VOICE_RATE * 2);
    le16(w + 32, 2);
    le16(w + 34, 16);
    memcpy(w + 36, "data", 4);
    le32(w + 40, pcm_bytes);
    ok = sink(w, sizeof w, user);
    uint8_t blk[ILF_ADPCM_BLOCK_BYTES];
    int16_t pcm[ILF_ADPCM_BLOCK_SAMPLES];
    for (uint32_t done = 0; ok && done + ILF_ADPCM_BLOCK_BYTES <= h.len; done += ILF_ADPCM_BLOCK_BYTES) {
        if (fread(blk, 1, sizeof blk, f) != sizeof blk) { ok = false; break; }
        ilf_adpcm_decode_block(blk, pcm);
        ok = sink(pcm, sizeof pcm, user);
    }
    fclose(f);
    return ok;
}

void ilf_voice_uploaded(const ilf_upload_t *u)
{
    lock();
    if (!u->voice) {
        remove(u->path);   /* рисунок на барашке не посмотреть — место нужнее */
        unlock();
        return;
    }
    FILE *f = fopen(u->path, "r+b");
    file_hdr_t h = { 0 };
    uint32_t off;
    if (f && read_hdr(f, &h, &off)) {
        h.flags |= F_UPLOADED;
        fseek(f, 0, SEEK_SET);
        fwrite(&h, HDR1_SIZE, 1, f);
    }
    if (f) fclose(f);
    if (h.flags & F_MINE) {
        remove(u->path);
    } else {
        for (int i = 0; i < s_count; i++) if (s_items[i].seq == u->seq) s_items[i].flags |= F_UPLOADED;
        /* оставить последние ILF_VOICE_KEEP_SYNCED; непрослушанные не трогаем */
        int kept = 0;
        for (int i = s_count - 1; i >= 0; i--) {
            entry_t *e = &s_items[i];
            if (!(e->flags & F_UPLOADED) || !e->played || kept < ILF_VOICE_KEEP_SYNCED) {
                kept += (e->flags & F_UPLOADED) ? 1 : 0;
                continue;
            }
            char p[64];
            path_of('v', e->seq, p, sizeof p);
            remove(p);
            memmove(s_items + i, s_items + i + 1, sizeof(entry_t) * (size_t)(s_count - i - 1));
            s_count--;
        }
    }
    unlock();
}

/* ---------- запуск ---------- */

bool ilf_voice_on_sd(void) { return s_sd; }

void ilf_voice_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    esp_vfs_spiffs_conf_t conf = { .base_path = FLASH_DIR, .partition_label = "storage",
                                   .max_files = 4, .format_if_mount_failed = true };
    bool flash_ok = esp_vfs_spiffs_register(&conf) == ESP_OK;
    if (ilf_sd_mount(SD_MOUNT) == ESP_OK) {
        mkdir(SD_DIR, 0775);
        struct stat st;
        if (stat(SD_DIR, &st) == 0) {
            s_sd = true;
            s_dir = SD_DIR;
        }
    }
    s_fs_ok = s_sd || flash_ok;
    if (s_fs_ok) {
        lock();
        scan_locked();
        unlock();
    } else {
        ESP_LOGW(TAG, "хранилище не поднялось, голосовые не сохранятся");
    }
    ESP_LOGI(TAG, "голосовые: %s, в списке %d", s_sd ? "карта microSD" : "внутренняя память", s_count);
    s_cmd = xQueueCreate(4, sizeof(cmd_t));
    s_live_out = xQueueCreate(8, ILF_ADPCM_BLOCK_BYTES);
    s_live_in = xQueueCreate(24, sizeof(live_blk_t));
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 0);
}
