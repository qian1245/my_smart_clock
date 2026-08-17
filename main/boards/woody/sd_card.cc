#include "sd_card.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "SdCard";

// Pin definitions matching Woody board schematics
#define SD_CS_PIN             GPIO_NUM_4
#define SD_MOSI_PIN           GPIO_NUM_5
#define SD_SCK_PIN            GPIO_NUM_6
#define SD_MISO_PIN           GPIO_NUM_7
#define SD_DET_PIN            GPIO_NUM_16

static sdmmc_card_t *s_card = nullptr;
static bool s_spi_bus_initialized = false;

esp_err_t sd_card_init(void) {
    // Configure Detection Pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SD_DET_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Boost Drive Strength for SPI pins (CS, MOSI, SCK)
    gpio_set_drive_capability(SD_CS_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SD_MOSI_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SD_SCK_PIN, GPIO_DRIVE_CAP_3);

    return ESP_OK;
}

bool sd_card_is_inserted(void) {
    // SD_DET_PIN goes low when card is inserted
    return gpio_get_level(SD_DET_PIN) == 0;
}

esp_err_t sd_card_mount(void) {
    if (s_card != nullptr) {
        return ESP_OK; // Already mounted
    }

    // 【核心修复】：在尝试挂载前先通过检测引脚判断。如果没插卡，直接返回，
    // 避免调用 esp_vfs_fat_sdspi_mount() 触发 IDF 底层在超时清理时的队列断言崩溃。
    if (!sd_card_is_inserted()) {
        ESP_LOGW(TAG, "No SD card detected in slot. Skipping mount.");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "SD Card detected, mounting on SPI2...");

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
        .disk_status_check_enable = false
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000; // 20MHz for data transfer
    host.flags = SDMMC_HOST_FLAG_SPI;

    if (!s_spi_bus_initialized) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_MOSI_PIN,
            .miso_io_num = SD_MISO_PIN,
            .sclk_io_num = SD_SCK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
            .flags = SPICOMMON_BUSFLAG_MASTER,
            .intr_flags = 0
        };

        gpio_pullup_en(SD_MISO_PIN);

        esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI2 bus: %s", esp_err_to_name(ret));
            return ret;
        }
        s_spi_bus_initialized = true;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = SPI2_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem: %s", esp_err_to_name(ret));
        s_card = nullptr;
        return ret;
    }

    ESP_LOGI(TAG, "SD Card mounted successfully at %s", BSP_SD_MOUNT_POINT);

    // Create animation folder if it doesn't exist
    struct stat st;
    if (stat(ANIMATION_DIR, &st) != 0) {
        ESP_LOGI(TAG, "%s folder not found, creating...", ANIMATION_DIR);
        mkdir(ANIMATION_DIR, 0777);
    }

    return ESP_OK;
}

esp_err_t sd_card_unmount(void) {
    if (s_card == nullptr) {
        return ESP_OK;
    }
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    ESP_LOGI(TAG, "SD Card unmounted");
    return ret;
}

bool sd_card_file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0);
}