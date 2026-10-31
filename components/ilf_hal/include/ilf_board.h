/*
 * ILF OS — HAL платы Waveshare ESP32-S3-Touch-AMOLED-1.8.
 *
 * Пины сверены с официальными примерами Waveshare
 * (github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8). Схемы платы в открытом
 * доступе нет, поэтому всё проверяем на живой плате.
 *
 * Бывает две ревизии платы, прошивка определяет её сама по адресу тача:
 *   V1: экран SH8601 + тач FT3168 (I2C 0x38)
 *   V2: экран CO5300 + тач CST820 (I2C 0x15)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- общая шина I2C: тач, гироскоп, часы, питание, расширитель --- */
#define ILF_I2C_PORT        I2C_NUM_0
#define ILF_I2C_SDA         GPIO_NUM_15
#define ILF_I2C_SCL         GPIO_NUM_14
#define ILF_I2C_HZ          400000

#define ILF_ADDR_TCA9554    0x20  /* расширитель: сброс экрана и тача, питание экрана */
#define ILF_ADDR_FT3168     0x38
#define ILF_ADDR_CST820     0x15
#define ILF_ADDR_QMI8658_A  0x6B
#define ILF_ADDR_QMI8658_B  0x6A
#define ILF_ADDR_PCF85063   0x51
#define ILF_ADDR_AXP2101    0x34

/* --- экран AMOLED 368×448 по QSPI --- */
#define ILF_LCD_W           368
#define ILF_LCD_H           448
#define ILF_LCD_CS          GPIO_NUM_12
#define ILF_LCD_SCLK        GPIO_NUM_11
#define ILF_LCD_D0          GPIO_NUM_4
#define ILF_LCD_D1          GPIO_NUM_5
#define ILF_LCD_D2          GPIO_NUM_6
#define ILF_LCD_D3          GPIO_NUM_7
#define ILF_LCD_SPI_HOST    SPI2_HOST
#define ILF_LCD_PCLK_HZ     (40 * 1000 * 1000)

#define ILF_TOUCH_INT       GPIO_NUM_21
#define ILF_BOOT_BUTTON     GPIO_NUM_0

/* --- звук (ES8311), пока не используется: голосовые в v0.2 --- */
#define ILF_I2S_MCLK        GPIO_NUM_16
#define ILF_I2S_BCLK        GPIO_NUM_9
#define ILF_I2S_WS          GPIO_NUM_45
#define ILF_I2S_DOUT        GPIO_NUM_8
#define ILF_I2S_DIN         GPIO_NUM_10

/* биты расширителя TCA9554 */
#define ILF_EXIO_LCD_RST    (1u << 0)
#define ILF_EXIO_LCD_PWR    (1u << 1)
#define ILF_EXIO_TOUCH_RST  (1u << 2)
#define ILF_EXIO_SD_CS      (1u << 7)

typedef enum {
    ILF_BOARD_UNKNOWN = 0,
    ILF_BOARD_V1_SH8601_FT3168,
    ILF_BOARD_V2_CO5300_CST820,
} ilf_board_rev_t;

/* Шина I2C, сброс экрана/тача через расширитель, определение ревизии. */
esp_err_t ilf_board_init(void);
ilf_board_rev_t ilf_board_rev(void);
const char *ilf_board_rev_name(void);
i2c_master_bus_handle_t ilf_board_i2c(void);

/* Добавить устройство на общую шину. */
esp_err_t ilf_board_i2c_add(uint8_t addr, i2c_master_dev_handle_t *out);
esp_err_t ilf_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
esp_err_t ilf_i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);

/* --- экран --- */
/* Поднять экран и LVGL. Возвращает дисплей LVGL. */
lv_display_t *ilf_display_init(void);
void ilf_display_set_brightness(uint8_t level); /* 0..255, 0 = экран выключен */

/* --- тач --- */
esp_err_t ilf_touch_init(lv_display_t *disp);
/* было ли касание после последнего вызова (для пробуждения) */
bool ilf_touch_take_wake(void);

/* --- гироскоп/акселерометр QMI8658 --- */
typedef struct {
    float ax, ay, az;  /* ускорение в g */
} ilf_accel_t;
esp_err_t ilf_imu_init(void);
esp_err_t ilf_imu_read(ilf_accel_t *out);

/* --- часы реального времени PCF85063 --- */
#include <time.h>
esp_err_t ilf_rtc_init(void);
esp_err_t ilf_rtc_get(struct tm *utc);
esp_err_t ilf_rtc_set(const struct tm *utc);

/* --- питание AXP2101 --- */
esp_err_t ilf_power_init(void);
esp_err_t ilf_power_battery(uint8_t *percent, bool *charging);

/* --- кнопка BOOT --- */
esp_err_t ilf_button_init(void);
bool ilf_button_pressed(void);

#ifdef __cplusplus
}
#endif
