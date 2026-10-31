#include "ilf_adpcm.h"

static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t index_table[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* общий шаг декодера: по коду обновляет предсказание и индекс шага */
static void apply(int *pred, int *index, uint8_t code)
{
    int step = step_table[*index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    *pred = clamp(code & 8 ? *pred - diff : *pred + diff, -32768, 32767);
    *index = clamp(*index + index_table[code], 0, 88);
}

static uint8_t encode_one(int *pred, int *index, int sample)
{
    int step = step_table[*index];
    int diff = sample - *pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) code |= 1;
    apply(pred, index, code);
    return code;
}

void ilf_adpcm_encode_block(ilf_adpcm_state_t *st, const int16_t *pcm, uint8_t *block)
{
    int pred = st->predicted, index = st->index;
    block[0] = (uint8_t)(pred & 0xFF);
    block[1] = (uint8_t)((uint16_t)pred >> 8);
    block[2] = (uint8_t)index;
    uint8_t *out = block + ILF_ADPCM_HEADER;
    for (int i = 0; i < ILF_ADPCM_BLOCK_SAMPLES; i += 2) {
        uint8_t lo = encode_one(&pred, &index, pcm[i]);
        uint8_t hi = encode_one(&pred, &index, pcm[i + 1]);
        *out++ = (uint8_t)(lo | (hi << 4));
    }
    st->predicted = (int16_t)pred;
    st->index = (int8_t)index;
}

void ilf_adpcm_decode_block(const uint8_t *block, int16_t *pcm)
{
    int pred = (int16_t)(block[0] | (block[1] << 8));
    int index = clamp(block[2], 0, 88);
    const uint8_t *in = block + ILF_ADPCM_HEADER;
    for (int i = 0; i < ILF_ADPCM_BLOCK_SAMPLES; i += 2) {
        uint8_t b = *in++;
        apply(&pred, &index, b & 0x0F);
        pcm[i] = (int16_t)pred;
        apply(&pred, &index, b >> 4);
        pcm[i + 1] = (int16_t)pred;
    }
}
