/*
 * Тач: FT3168 (V1) или CST820 (V2). У обоих координаты лежат начиная с регистра 0x02:
 *   [0x02] число касаний, [0x03] X старший (4 бита), [0x04] X младший,
 *   [0x05] Y старший (4 бита), [0x06] Y младший.
 */
#include "ilf_board.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ilf_touch";

static i2c_master_dev_handle_t s_dev;
static volatile bool s_wake;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t b[5];
    if (ilf_i2c_read_regs(s_dev, 0x02, b, sizeof b) == ESP_OK && (b[0] & 0x0F) > 0) {
        int x = ((b[1] & 0x0F) << 8) | b[2];
        int y = ((b[3] & 0x0F) << 8) | b[4];
        if (x >= ILF_LCD_W) x = ILF_LCD_W - 1;
        if (y >= ILF_LCD_H) y = ILF_LCD_H - 1;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        s_wake = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t ilf_touch_init(lv_display_t *disp)
{
    uint8_t addr = ilf_board_rev() == ILF_BOARD_V2_CO5300_CST820 ? ILF_ADDR_CST820 : ILF_ADDR_FT3168;
    ESP_RETURN_ON_ERROR(ilf_board_i2c_add(addr, &s_dev), TAG, "touch add");
    if (addr == ILF_ADDR_FT3168) {
        ilf_i2c_write_reg(s_dev, 0x00, 0x00); /* рабочий режим */
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_display(indev, disp);
    ESP_LOGI(TAG, "тач 0x%02x готов", addr);
    return ESP_OK;
}

bool ilf_touch_take_wake(void)
{
    bool w = s_wake;
    s_wake = false;
    return w;
}
