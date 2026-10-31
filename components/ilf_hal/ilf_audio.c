/*
 * Звук: кодек ES8311 (I2C 0x18) + I2S, 8 кГц, 16 бит, моно. Микрофон и динамик.
 *
 * Последовательность регистров повторяет драйвер es8311 из esp_codec_dev
 * (Espressif) для режима ведомого с MCLK = 256 × частота. На живой плате
 * проверить: громкость (ILF_AUDIO_VOLUME), усиление микрофона и пин усилителя.
 */
#include "ilf_board.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ilf_audio";

#define ES8311_ADDR       0x18
#define ILF_AUDIO_VOLUME  0xBF   /* 0xBF = 0 дБ, шаг 0.5 дБ */
#define ILF_MIC_GAIN      0x1A   /* аналоговое усиление микрофона (рег. 0x14) */
#define ILF_MIC_DIGITAL   0xC8   /* цифровая громкость АЦП (рег. 0x17) */

static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_tx, s_rx;
static bool s_ok;

static esp_err_t wr(uint8_t reg, uint8_t val) { return ilf_i2c_write_reg(s_codec, reg, val); }

static esp_err_t codec_init(void)
{
    static const uint8_t seq[][2] = {
        { 0x45, 0x00 },
        { 0x01, 0x30 }, { 0x02, 0x10 },
        /* тактирование для MCLK = 256·fs: делители 1, OSR 0x10 */
        { 0x02, 0x00 }, { 0x05, 0x00 }, { 0x03, 0x10 }, { 0x04, 0x10 },
        { 0x07, 0x00 }, { 0x08, 0xFF }, { 0x06, 0x03 },
        { 0x16, 0x24 }, { 0x0B, 0x00 }, { 0x0C, 0x00 },
        { 0x10, 0x1F }, { 0x11, 0x7F },
        { 0x00, 0x80 },                      /* включить, режим ведомого */
        { 0x01, 0x3F },                      /* все такты включены */
        { 0x09, 0x0C }, { 0x0A, 0x0C },      /* I2S, 16 бит, вход и выход */
        { 0x0D, 0x01 }, { 0x0E, 0x02 },      /* питание аналоговой части, АЦП */
        { 0x12, 0x00 }, { 0x13, 0x10 },      /* ЦАП и выход на наушники/усилитель */
        { 0x14, ILF_MIC_GAIN },
        { 0x15, 0x40 }, { 0x1B, 0x0A }, { 0x1C, 0x6A },
        { 0x17, ILF_MIC_DIGITAL },
        { 0x37, 0x08 },
        { 0x32, ILF_AUDIO_VOLUME },
    };
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        ESP_RETURN_ON_ERROR(wr(seq[i][0], seq[i][1]), TAG, "ES8311 reg 0x%02x", seq[i][0]);
    }
    return ESP_OK;
}

esp_err_t ilf_audio_init(void)
{
    ESP_RETURN_ON_ERROR(ilf_board_i2c_add(ES8311_ADDR, &s_codec), TAG, "ES8311 add");

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_tx, &s_rx), TAG, "i2s channel");
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ILF_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = ILF_I2S_MCLK, .bclk = ILF_I2S_BCLK, .ws = ILF_I2S_WS,
            .dout = ILF_I2S_DOUT, .din = ILF_I2S_DIN,
        },
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &sc), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &sc), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");

    vTaskDelay(pdMS_TO_TICKS(10)); /* кодеку нужен MCLK до настройки */
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec");

    gpio_config_t pa = { .pin_bit_mask = 1ULL << ILF_PA_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pa);
    gpio_set_level(ILF_PA_EN, 0);
    s_ok = true;
    ESP_LOGI(TAG, "ES8311 готов, %d Гц", ILF_AUDIO_RATE);
    return ESP_OK;
}

bool ilf_audio_ok(void) { return s_ok; }

void ilf_audio_speaker(bool on)
{
    if (s_ok) gpio_set_level(ILF_PA_EN, on ? 1 : 0);
}

size_t ilf_audio_read(int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (!s_ok) return 0;
    size_t got = 0;
    i2s_channel_read(s_rx, pcm, samples * sizeof(int16_t), &got, pdMS_TO_TICKS(timeout_ms));
    return got / sizeof(int16_t);
}

size_t ilf_audio_write(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (!s_ok) return 0;
    size_t put = 0;
    i2s_channel_write(s_tx, pcm, samples * sizeof(int16_t), &put, pdMS_TO_TICKS(timeout_ms));
    return put / sizeof(int16_t);
}
