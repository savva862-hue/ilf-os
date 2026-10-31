/*
 * Гироскоп QMI8658 (только акселерометр), часы PCF85063, питание AXP2101, кнопка BOOT.
 * Регистры — по даташитам; всё помечено, чтобы было легко сверить на живой плате.
 */
#include "ilf_board.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ilf_sensors";

/* ---------------- QMI8658 ---------------- */
#define QMI_WHO_AM_I   0x00   /* = 0x05 */
#define QMI_CTRL1      0x02
#define QMI_CTRL2      0x03   /* акселерометр: шкала и частота */
#define QMI_CTRL7      0x08   /* включение датчиков */
#define QMI_AX_L       0x35
#define QMI_RESET      0x60

static i2c_master_dev_handle_t s_imu;
static float s_lsb_per_g = 8192.f; /* ±4g */

esp_err_t ilf_imu_init(void)
{
    const uint8_t addrs[2] = {ILF_ADDR_QMI8658_A, ILF_ADDR_QMI8658_B};
    for (int i = 0; i < 2 && !s_imu; i++) {
        if (i2c_master_probe(ilf_board_i2c(), addrs[i], 50) != ESP_OK) continue;
        ESP_RETURN_ON_ERROR(ilf_board_i2c_add(addrs[i], &s_imu), TAG, "imu add");
    }
    ESP_RETURN_ON_FALSE(s_imu, ESP_ERR_NOT_FOUND, TAG, "QMI8658 не найден");

    ilf_i2c_write_reg(s_imu, QMI_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t who = 0;
    ilf_i2c_read_regs(s_imu, QMI_WHO_AM_I, &who, 1);
    if (who != 0x05) ESP_LOGW(TAG, "WHO_AM_I = 0x%02x, ждали 0x05", who);

    ESP_RETURN_ON_ERROR(ilf_i2c_write_reg(s_imu, QMI_CTRL1, 0x60), TAG, "ctrl1"); /* автоинкремент адреса */
    ESP_RETURN_ON_ERROR(ilf_i2c_write_reg(s_imu, QMI_CTRL2, 0x16), TAG, "ctrl2"); /* ±4g, ~117 Гц */
    ESP_RETURN_ON_ERROR(ilf_i2c_write_reg(s_imu, QMI_CTRL7, 0x01), TAG, "ctrl7"); /* только акселерометр */
    ESP_LOGI(TAG, "QMI8658 готов");
    return ESP_OK;
}

esp_err_t ilf_imu_read(ilf_accel_t *out)
{
    if (!s_imu) return ESP_ERR_INVALID_STATE;
    uint8_t b[6];
    ESP_RETURN_ON_ERROR(ilf_i2c_read_regs(s_imu, QMI_AX_L, b, 6), TAG, "read");
    out->ax = (int16_t)(b[1] << 8 | b[0]) / s_lsb_per_g;
    out->ay = (int16_t)(b[3] << 8 | b[2]) / s_lsb_per_g;
    out->az = (int16_t)(b[5] << 8 | b[4]) / s_lsb_per_g;
    return ESP_OK;
}

/* ---------------- PCF85063 ---------------- */
static i2c_master_dev_handle_t s_rtc;

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

esp_err_t ilf_rtc_init(void)
{
    ESP_RETURN_ON_ERROR(ilf_board_i2c_add(ILF_ADDR_PCF85063, &s_rtc), TAG, "rtc add");
    return ilf_i2c_write_reg(s_rtc, 0x00, 0x00); /* часы идут, 24-часовой режим */
}

esp_err_t ilf_rtc_get(struct tm *t)
{
    if (!s_rtc) return ESP_ERR_INVALID_STATE;
    uint8_t b[7];
    ESP_RETURN_ON_ERROR(ilf_i2c_read_regs(s_rtc, 0x04, b, 7), TAG, "rtc read");
    if (b[0] & 0x80) return ESP_ERR_INVALID_STATE; /* флаг OS: время сбилось, батарейку отключали */
    *t = (struct tm){0};
    t->tm_sec  = bcd2bin(b[0] & 0x7F);
    t->tm_min  = bcd2bin(b[1] & 0x7F);
    t->tm_hour = bcd2bin(b[2] & 0x3F);
    t->tm_mday = bcd2bin(b[3] & 0x3F);
    t->tm_wday = b[4] & 0x07;
    t->tm_mon  = bcd2bin(b[5] & 0x1F) - 1;
    t->tm_year = bcd2bin(b[6]) + 100;
    return ESP_OK;
}

esp_err_t ilf_rtc_set(const struct tm *t)
{
    if (!s_rtc) return ESP_ERR_INVALID_STATE;
    uint8_t b[8] = {
        0x04,
        bin2bcd(t->tm_sec), bin2bcd(t->tm_min), bin2bcd(t->tm_hour), bin2bcd(t->tm_mday),
        (uint8_t)t->tm_wday, bin2bcd(t->tm_mon + 1), bin2bcd(t->tm_year % 100),
    };
    return i2c_master_transmit(s_rtc, b, sizeof b, 50);
}

/* ---------------- AXP2101 ---------------- */
static i2c_master_dev_handle_t s_pmu;

esp_err_t ilf_power_init(void)
{
    ESP_RETURN_ON_ERROR(ilf_board_i2c_add(ILF_ADDR_AXP2101, &s_pmu), TAG, "pmu add");
    uint8_t v = 0;
    /* включить измерение напряжения батареи (0x30 бит 0) и топливомер (0x18 бит 3) */
    if (ilf_i2c_read_regs(s_pmu, 0x30, &v, 1) == ESP_OK) ilf_i2c_write_reg(s_pmu, 0x30, v | 0x01);
    if (ilf_i2c_read_regs(s_pmu, 0x18, &v, 1) == ESP_OK) ilf_i2c_write_reg(s_pmu, 0x18, v | 0x08);
    return ESP_OK;
}

esp_err_t ilf_power_battery(uint8_t *percent, bool *charging)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    uint8_t pct = 0, st = 0;
    ESP_RETURN_ON_ERROR(ilf_i2c_read_regs(s_pmu, 0xA4, &pct, 1), TAG, "battery %%");
    ESP_RETURN_ON_ERROR(ilf_i2c_read_regs(s_pmu, 0x01, &st, 1), TAG, "status");
    *percent = pct > 100 ? 100 : pct;
    *charging = ((st >> 5) & 0x03) == 0x01;
    return ESP_OK;
}

/* ---------------- кнопка BOOT ---------------- */
esp_err_t ilf_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ILF_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&cfg);
}

bool ilf_button_pressed(void) { return gpio_get_level(ILF_BOOT_BUTTON) == 0; }
