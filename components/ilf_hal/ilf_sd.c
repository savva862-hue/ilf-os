/*
 * Карта microSD: SDMMC в однобитном режиме. DAT3/CS карты держит в единице
 * расширитель TCA9554 (бит ILF_EXIO_SD_CS, выставляется в ilf_board_init).
 * Пины — по схеме Waveshare ESP32-S3-Touch-AMOLED-1.8; сверить на живой плате.
 * Нет карты — не беда: голосовые лягут во внутреннюю память.
 */
#include "ilf_board.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "ilf_sd";
static sdmmc_card_t *s_card;

esp_err_t ilf_sd_mount(const char *base)
{
    if (s_card) return ESP_OK;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   /* 20 МГц: голосу хватает с запасом */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = ILF_SD_CLK;
    slot.cmd = ILF_SD_CMD;
    slot.d0 = ILF_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = false,   /* чужую карту не стираем */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(base, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "карты нет или она не читается (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "карта %s, %llu МБ", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

bool ilf_sd_ok(void) { return s_card != NULL; }
