#include "ilf_voice.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ilf_adpcm.h"
#include "ilf_board.h"
#include "ilf_herd.h"

static const char *TAG = "ilf_voice";

#define BASE   "/store"
#define MAGIC  0x56464C49u   /* "ILFV" */

typedef struct {
    uint32_t magic;
    uint8_t  mac[6];
    uint8_t  played;
    uint8_t  pad;
    uint32_t len;
    uint32_t time;
} file_hdr_t;

typedef struct {
    uint32_t seq;
    uint8_t  mac[6];
    bool     played;
    uint32_t len;
} entry_t;

static entry_t s_items[ILF_VOICE_KEEP];   /* по возрастанию seq: последний — самый новый */
static int s_count;
static uint32_t s_next_seq = 1;
static bool s_fs_ok;

/* ---------- задача звука ---------- */

typedef enum { CMD_REC_START, CMD_REC_STOP, CMD_PLAY } cmd_type_t;
typedef struct { cmd_type_t type; uint8_t *data; size_t len; } cmd_t;

static QueueHandle_t s_cmd;
static volatile bool s_recording, s_playing;
static volatile size_t s_rec_blocks;
static uint8_t *volatile s_done;
static volatile size_t s_done_len;

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

static void record(void)
{
    uint8_t *buf = big_alloc(ILF_HERD_MAX_MESSAGE);
    if (!buf) { s_recording = false; return; }
    int16_t pcm[ILF_ADPCM_BLOCK_SAMPLES];
    ilf_adpcm_state_t st = { 0 };
    size_t max_blocks = ILF_HERD_MAX_MESSAGE / ILF_ADPCM_BLOCK_BYTES;
    s_rec_blocks = 0;
    /* первые 60 мс — щелчок кнопки и раскачка микрофона, их выбрасываем */
    ilf_audio_read(pcm, ILF_VOICE_RATE * 60 / 1000, 100);
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmd, &c, 0) == pdTRUE && c.type == CMD_REC_STOP) break;
        if (s_rec_blocks >= max_blocks) break;   /* ~17 секунд — хватит */
        size_t got = ilf_audio_read(pcm, ILF_ADPCM_BLOCK_SAMPLES, 200);
        if (got < ILF_ADPCM_BLOCK_SAMPLES) memset(pcm + got, 0, (ILF_ADPCM_BLOCK_SAMPLES - got) * sizeof(int16_t));
        ilf_adpcm_encode_block(&st, pcm, buf + s_rec_blocks * ILF_ADPCM_BLOCK_BYTES);
        s_rec_blocks++;
    }
    s_recording = false;
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

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmd, &c, portMAX_DELAY) != pdTRUE) continue;
        ilf_audio_begin();
        if (c.type == CMD_REC_START) record();
        else if (c.type == CMD_PLAY) play(c.data, c.len);
        ilf_audio_end();
    }
}

void ilf_voice_record_start(void)
{
    if (s_recording || s_playing || !ilf_audio_ok()) return;
    s_recording = true;
    s_rec_blocks = 0;
    cmd_t c = { .type = CMD_REC_START };
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

/* ---------- хранилище ---------- */

static void path_of(uint32_t seq, char *out, size_t n) { snprintf(out, n, BASE "/v%05lu.ilf", (unsigned long)seq); }

static int cmp_seq(const void *a, const void *b)
{
    uint32_t x = ((const entry_t *)a)->seq, y = ((const entry_t *)b)->seq;
    return x < y ? -1 : x > y;
}

static void scan(void)
{
    DIR *d = opendir(BASE);
    if (!d) return;
    struct dirent *e;
    entry_t all[ILF_VOICE_KEEP * 2];
    int n = 0;
    while ((e = readdir(d)) && n < (int)(sizeof all / sizeof all[0])) {
        unsigned long seq;
        if (sscanf(e->d_name, "v%5lu.ilf", &seq) != 1) continue;
        char p[64];
        path_of((uint32_t)seq, p, sizeof p);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        file_hdr_t h;
        bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == MAGIC;
        fclose(f);
        if (!ok) { remove(p); continue; }
        all[n] = (entry_t){ .seq = (uint32_t)seq, .played = h.played, .len = h.len };
        memcpy(all[n].mac, h.mac, 6);
        n++;
    }
    closedir(d);
    qsort(all, (size_t)n, sizeof all[0], cmp_seq);
    int start = n > ILF_VOICE_KEEP ? n - ILF_VOICE_KEEP : 0;
    for (int i = 0; i < start; i++) {
        char p[64];
        path_of(all[i].seq, p, sizeof p);
        remove(p);
    }
    s_count = n - start;
    memcpy(s_items, all + start, sizeof(entry_t) * (size_t)s_count);
    if (s_count) s_next_seq = s_items[s_count - 1].seq + 1;
}

void ilf_voice_store(const uint8_t mac[6], const uint8_t *data, size_t len)
{
    if (!s_fs_ok) return;
    if (s_count == ILF_VOICE_KEEP) {
        char p[64];
        path_of(s_items[0].seq, p, sizeof p);
        remove(p);
        memmove(s_items, s_items + 1, sizeof(entry_t) * (ILF_VOICE_KEEP - 1));
        s_count--;
    }
    uint32_t seq = s_next_seq++;
    if (s_next_seq > 99999) s_next_seq = 1;
    char p[64];
    path_of(seq, p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) { ESP_LOGW(TAG, "не записать %s", p); return; }
    file_hdr_t h = { .magic = MAGIC, .len = (uint32_t)len, .time = (uint32_t)time(NULL) };
    memcpy(h.mac, mac, 6);
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(data, 1, len, f) == len;
    fclose(f);
    if (!ok) { remove(p); ESP_LOGW(TAG, "место кончилось"); return; }
    entry_t *e = &s_items[s_count++];
    *e = (entry_t){ .seq = seq, .played = false, .len = (uint32_t)len };
    memcpy(e->mac, mac, 6);
}

int ilf_voice_items(ilf_ui_voice_item_t *out, int max, ilf_voice_name_fn name)
{
    int n = 0;
    for (int i = s_count - 1; i >= 0 && n < max; i--, n++) {
        const entry_t *e = &s_items[i];
        const char *nm = name ? name(e->mac) : NULL;
        snprintf(out[n].from, sizeof out[n].from, "%s", nm ? nm : "Барашек");
        out[n].seconds = (uint16_t)((e->len / ILF_ADPCM_BLOCK_BYTES) * ILF_ADPCM_BLOCK_SAMPLES / ILF_VOICE_RATE);
        out[n].unplayed = !e->played;
    }
    return n;
}

bool ilf_voice_play(int item)
{
    int i = s_count - 1 - item;
    if (i < 0 || s_playing || s_recording || !ilf_audio_ok()) return false;
    entry_t *e = &s_items[i];
    char p[64];
    path_of(e->seq, p, sizeof p);
    FILE *f = fopen(p, "r+b");
    if (!f) return false;
    uint8_t *buf = big_alloc(e->len);
    file_hdr_t h;
    bool ok = buf && fread(&h, sizeof h, 1, f) == 1 && fread(buf, 1, e->len, f) == e->len;
    if (ok && !h.played) {
        h.played = 1;
        fseek(f, 0, SEEK_SET);
        fwrite(&h, sizeof h, 1, f);
    }
    fclose(f);
    if (!ok) { free(buf); return false; }
    e->played = true;
    s_playing = true;
    cmd_t c = { .type = CMD_PLAY, .data = buf, .len = e->len };
    xQueueSend(s_cmd, &c, 0);
    return true;
}

void ilf_voice_init(void)
{
    esp_vfs_spiffs_conf_t conf = { .base_path = BASE, .partition_label = "storage",
                                   .max_files = 4, .format_if_mount_failed = true };
    s_fs_ok = esp_vfs_spiffs_register(&conf) == ESP_OK;
    if (s_fs_ok) scan();
    else ESP_LOGW(TAG, "хранилище не поднялось, голосовые не сохранятся");
    ESP_LOGI(TAG, "голосовых в памяти: %d", s_count);
    s_cmd = xQueueCreate(4, sizeof(cmd_t));
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 0);
}
