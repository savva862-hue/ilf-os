/*
 * Экран AMOLED 368×448 по QSPI + LVGL.
 * Свой небольшой драйвер для обеих ревизий (SH8601 и CO5300): у них одинаковый
 * QSPI-протокол, отличаются только команды запуска и сдвиг по X у CO5300.
 */
#include "ilf_board.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ilf_display";

#define QSPI_CMD_WRITE_REG   0x02
#define QSPI_CMD_WRITE_COLOR 0x32
#define LCD_CMD(op, reg)     (((uint32_t)(op) << 24) | ((uint32_t)(reg) << 8))
#define BUF_LINES            48

typedef struct {
    uint8_t cmd;
    uint8_t data[4];
    uint8_t len;
    uint16_t delay_ms;
} init_cmd_t;

/* Последовательности запуска взяты из примеров Waveshare и esp-iot-solution. */
static const init_cmd_t s_init_sh8601[] = {
    {0x11, {0}, 0, 120},                      /* выйти из сна */
    {0x44, {0x01, 0xD1}, 2, 0},               /* линия tearing effect */
    {0x35, {0x00}, 1, 0},                     /* включить TE */
    {0x53, {0x20}, 1, 0},                     /* управление яркостью */
    {0x3A, {0x55}, 1, 0},                     /* 16 бит на пиксель */
    {0x2A, {0x00, 0x00, 0x01, 0x6F}, 4, 0},   /* столбцы 0..367 */
    {0x2B, {0x00, 0x00, 0x01, 0xBF}, 4, 0},   /* строки 0..447 */
    {0x51, {0x00}, 1, 0},
    {0x29, {0}, 0, 10},                       /* экран включён */
};

static const init_cmd_t s_init_co5300[] = {
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},                     /* режим SPI */
    {0x3A, {0x55}, 1, 0},
    {0x35, {0x00}, 1, 0},
    {0x53, {0x20}, 1, 0},
    {0x51, {0x00}, 1, 0},
    {0x63, {0xFF}, 1, 0},
    {0x11, {0}, 0, 120},
    {0x29, {0}, 0, 20},
};

static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;
static int s_x_offset;

static esp_err_t write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, LCD_CMD(QSPI_CMD_WRITE_REG, reg), data, len);
}

static esp_err_t run_init(const init_cmd_t *cmds, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(write_reg(cmds[i].cmd, cmds[i].len ? cmds[i].data : NULL, cmds[i].len), TAG, "init cmd 0x%02x", cmds[i].cmd);
        if (cmds[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(cmds[i].delay_ms));
    }
    return ESP_OK;
}

static void set_window(int x1, int y1, int x2, int y2)
{
    x1 += s_x_offset;
    x2 += s_x_offset;
    const uint8_t col[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
    const uint8_t row[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
    write_reg(0x2A, col, 4);
    write_reg(0x2B, row, 4);
}

void ilf_display_set_brightness(uint8_t level)
{
    if (!s_io) return;
    write_reg(0x51, &level, 1);
}

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io; (void)edata;
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)disp;
    const uint32_t n = lv_area_get_size(area);
    lv_draw_sw_rgb565_swap(px, n); /* экран ждёт старший байт первым */
    set_window(area->x1, area->y1, area->x2, area->y2);
    esp_lcd_panel_io_tx_color(s_io, LCD_CMD(QSPI_CMD_WRITE_COLOR, 0x2C), px, n * 2);
}

/* Контроллер принимает окна только с чётным началом и нечётным концом. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

lv_display_t *ilf_display_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = ILF_LCD_SCLK,
        .data0_io_num = ILF_LCD_D0,
        .data1_io_num = ILF_LCD_D1,
        .data2_io_num = ILF_LCD_D2,
        .data3_io_num = ILF_LCD_D3,
        .max_transfer_sz = ILF_LCD_W * BUF_LINES * 2,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ILF_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(ILF_LCD_W, ILF_LCD_H);

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = ILF_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = ILF_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_trans_done,
        .user_ctx = s_disp,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = true },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ILF_LCD_SPI_HOST, &io_cfg, &s_io));

    if (ilf_board_rev() == ILF_BOARD_V2_CO5300_CST820) {
        s_x_offset = 16;
        ESP_ERROR_CHECK(run_init(s_init_co5300, sizeof s_init_co5300 / sizeof s_init_co5300[0]));
    } else {
        s_x_offset = 0;
        ESP_ERROR_CHECK(run_init(s_init_sh8601, sizeof s_init_sh8601 / sizeof s_init_sh8601[0]));
    }

    const size_t buf_bytes = ILF_LCD_W * BUF_LINES * 2;
    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    ilf_display_set_brightness(0xC0);
    ESP_LOGI(TAG, "экран готов, сдвиг X = %d", s_x_offset);
    return s_disp;
}
