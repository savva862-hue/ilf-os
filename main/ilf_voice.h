/*
 * Голосовые на плате: запись и проигрывание (отдельная задача), рация,
 * хранение на карте microSD (если вставлена) или во внутренней памяти,
 * и файлы для выгрузки на Яндекс Диск.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ilf_adpcm.h"
#include "ilf_ui.h"

#define ILF_VOICE_KEEP        30   /* сколько последних входящих голосовых хранить */
#define ILF_VOICE_KEEP_SYNCED  2   /* после выгрузки на Диск оставить на барашке столько последних */
#define ILF_VOICE_MAX_SECONDS 60

void ilf_voice_init(void);
bool ilf_voice_on_sd(void);

/* запись; live — рация: блоки сразу уходят в ilf_voice_live_take, без буфера */
void  ilf_voice_record_start(bool live);
void  ilf_voice_record_stop(void);
bool  ilf_voice_recording(void);
float ilf_voice_recorded_seconds(void);
/* Забрать готовую запись (один раз). Буфер освобождать через free(). */
bool  ilf_voice_take_recording(uint8_t **data, size_t *len);

/* рация */
bool ilf_voice_live_take(uint8_t block[ILF_ADPCM_BLOCK_BYTES]);  /* записанный блок для эфира */
void ilf_voice_live_feed(const uint8_t *block, size_t len);       /* блок от друга — в динамик */
bool ilf_voice_live_hearing(void);

/* Сохранить голосовое. mine — своё отправленное (в список не попадает, только на Диск). */
void ilf_voice_store(const uint8_t mac[6], const char *name, const uint8_t *data, size_t len, bool mine);
int  ilf_voice_items(ilf_ui_voice_item_t *out, int max);
bool ilf_voice_play(int item);   /* 0 = самое новое */
bool ilf_voice_playing(void);

/* Рисунок (RGB565) → BMP-файл для выгрузки. */
void ilf_voice_save_drawing(const uint16_t *px, int w, int h);

/* ---------- выгрузка ---------- */

typedef struct {
    char path[48];      /* файл на барашке */
    char name[96];      /* как назвать на Диске: «2026-10-31 14-05 Мира.wav» */
    bool voice;         /* голосовое (выгружается как WAV) или рисунок (BMP) */
    uint32_t seq;
    size_t size;        /* сколько байт уйдёт на Диск */
} ilf_upload_t;

/* Следующий файл, которого ещё нет на Диске. */
bool ilf_voice_next_upload(ilf_upload_t *u);
int  ilf_voice_pending_uploads(void);
/* Отдать содержимое для Диска кусками (голосовое — WAV 8 кГц). */
typedef bool (*ilf_voice_sink_fn)(const void *data, size_t len, void *user);
bool ilf_voice_read_upload(const ilf_upload_t *u, ilf_voice_sink_fn sink, void *user);
/* Файл на Диске: пометить и освободить место (оставить ILF_VOICE_KEEP_SYNCED последних). */
void ilf_voice_uploaded(const ilf_upload_t *u);
