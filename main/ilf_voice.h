/* Голосовые на плате: запись и проигрывание (отдельная задача), хранение входящих во flash. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ilf_ui.h"

#define ILF_VOICE_KEEP 30   /* сколько последних голосовых хранить */

void ilf_voice_init(void);

/* запись */
void  ilf_voice_record_start(void);
void  ilf_voice_record_stop(void);
bool  ilf_voice_recording(void);
float ilf_voice_recorded_seconds(void);
/* Забрать готовую запись (один раз). Буфер освобождать через free(). */
bool  ilf_voice_take_recording(uint8_t **data, size_t *len);

/* входящие */
void ilf_voice_store(const uint8_t mac[6], const uint8_t *data, size_t len);
typedef const char *(*ilf_voice_name_fn)(const uint8_t mac[6]);
int  ilf_voice_items(ilf_ui_voice_item_t *out, int max, ilf_voice_name_fn name);
bool ilf_voice_play(int item);   /* 0 = самое новое */
bool ilf_voice_playing(void);
