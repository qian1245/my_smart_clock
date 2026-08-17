#ifndef WOODY_ANIMATION_H
#define WOODY_ANIMATION_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

int animation_get_active_sequence(void);

#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*draw_callback_t)(const uint16_t *bitmap, int width, int height);

esp_err_t animation_init(draw_callback_t draw_cb);
void animation_deinit(void);
void animation_start(void);
void animation_stop(void);
bool animation_is_running(void);
void animation_enable_ble_init(void);
void animation_start_ble(void);
void animation_stop_ble(void);
bool animation_has_files(void);

#ifdef __cplusplus
}
#endif

#endif // WOODY_ANIMATION_H