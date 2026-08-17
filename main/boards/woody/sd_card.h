#ifndef WOODY_SD_CARD_H
#define WOODY_SD_CARD_H

#include "esp_err.h"
#include <stdbool.h>

#define BSP_SD_MOUNT_POINT "/sdcard"
#define ANIMATION_DIR      "/sdcard/animation"

esp_err_t sd_card_init(void);
esp_err_t sd_card_mount(void);
esp_err_t sd_card_unmount(void);
bool sd_card_is_inserted(void);
bool sd_card_file_exists(const char *path);

#endif // WOODY_SD_CARD_H
