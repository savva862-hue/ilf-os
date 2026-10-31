#include "ilf_board.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ilf_board";

static i2c_master_bus_handle_t s_bus;
static ilf_board_rev_t s_rev = ILF_BOARD_UNKNOWN;

i2c_master_bus_handle_t ilf_board_i2c(void) { return s_bus; }
ilf_board_rev_t ilf_board_rev(void) { return s_rev; }

const char *ilf_board_rev_name(void)
{
    switch (s_rev) {
    case ILF_BOARD_V1_SH8601_FT3168: return "V1 (SH8601 + FT3168)";
    case ILF_BOARD_V2_CO5300_CST820: return "V2 (CO5300 + CST820)";
    default: return "unknown";
    }
}

esp_err_t ilf_board_i2c_add(uint8_t addr, i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = ILF_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, out);
}

esp_err_t ilf_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof buf, 50);
}

esp_err_t ilf_i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 50);
}

static esp_err_t expander_reset_lcd_and_touch(void)
{
    i2c_master_dev_handle_t exio;
    ESP_RETURN_ON_ERROR(ilf_board_i2c_add(ILF_ADDR_TCA9554, &exio), TAG, "TCA9554 add");
    const uint8_t mask = ILF_EXIO_LCD_RST | ILF_EXIO_LCD_PWR | ILF_EXIO_TOUCH_RST | ILF_EXIO_SD_CS;
    esp_err_t err = ilf_i2c_write_reg(exio, 0x03, (uint8_t)~mask);       /* эти биты — выходы */
    if (err == ESP_OK) err = ilf_i2c_write_reg(exio, 0x01, ILF_EXIO_SD_CS); /* сброс экрана и тача */
    vTaskDelay(pdMS_TO_TICKS(20));
    if (err == ESP_OK) err = ilf_i2c_write_reg(exio, 0x01, mask);        /* питание + отпустить сброс */
    vTaskDelay(pdMS_TO_TICKS(150));
    i2c_master_bus_rm_device(exio);
    return err;
}

esp_err_t ilf_board_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ILF_I2C_PORT,
        .sda_io_num = ILF_I2C_SDA,
        .scl_io_num = ILF_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    if (expander_reset_lcd_and_touch() != ESP_OK) {
        ESP_LOGW(TAG, "TCA9554 не ответил, экран может не включиться");
    }

    if (i2c_master_probe(s_bus, ILF_ADDR_FT3168, 50) == ESP_OK) {
        s_rev = ILF_BOARD_V1_SH8601_FT3168;
    } else if (i2c_master_probe(s_bus, ILF_ADDR_CST820, 50) == ESP_OK) {
        s_rev = ILF_BOARD_V2_CO5300_CST820;
    }
    ESP_LOGI(TAG, "плата: %s", ilf_board_rev_name());
    return ESP_OK;
}
