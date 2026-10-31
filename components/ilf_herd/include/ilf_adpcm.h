/*
 * Сжатие голоса IMA ADPCM: 16 бит → 4 бита, 8 кГц моно = 4 КБ в секунду.
 *
 * Голосовое режется на независимые блоки по ILF_ADPCM_BLOCK_BYTES. Каждый блок
 * начинается с состояния декодера, поэтому его можно проиграть сам по себе:
 * потеря одного блока даёт короткий провал, а не шум до конца сообщения.
 * Один блок ровно влезает в один радиопакет стада.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ILF_VOICE_RATE          8000
#define ILF_ADPCM_HEADER         3                      /* предсказание (2) + шаг (1) */
#define ILF_ADPCM_BLOCK_SAMPLES 452
#define ILF_ADPCM_BLOCK_BYTES   (ILF_ADPCM_HEADER + ILF_ADPCM_BLOCK_SAMPLES / 2)  /* 229 */

typedef struct {
    int16_t predicted;
    int8_t  index;
} ilf_adpcm_state_t;

/* Сжать ровно ILF_ADPCM_BLOCK_SAMPLES отсчётов (хвост добить тишиной) в блок
   ILF_ADPCM_BLOCK_BYTES байт. Состояние переходит в следующий блок. */
void ilf_adpcm_encode_block(ilf_adpcm_state_t *st, const int16_t *pcm, uint8_t *block);

/* Распаковать блок в ILF_ADPCM_BLOCK_SAMPLES отсчётов. */
void ilf_adpcm_decode_block(const uint8_t *block, int16_t *pcm);

#ifdef __cplusplus
}
#endif
