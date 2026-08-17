#ifndef BLE_H
#define BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type for receiving BLE data
 */
typedef void (*ble_rx_callback_t)(uint8_t *data, uint16_t len);

/**
 * @brief Initialize BLE stack and GATT server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_init(void);

/**
 * @brief Deinitialize BLE stack, stop the host task, and release BT controller memory
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_deinit(void);

/**
 * @brief Start BLE advertising
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_start(void);

/**
 * @brief Stop BLE advertising and disconnect
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_stop(void);

/**
 * @brief Send data over BLE (Notify)
 * 
 * @param data Pointer to data buffer
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_send_data(uint8_t *data, uint16_t len);

/**
 * @brief Set the callback function for received data
 * 
 * @param callback The callback function
 */
void ble_set_rx_callback(ble_rx_callback_t callback);

/**
 * @brief Update BLE connection parameters (request high-speed or low-power mode)
 * 
 * @param high_speed true for high speed connection interval (7.5ms - 15ms), false for normal
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ble_update_conn_params(bool high_speed);

/**
 * @brief Check if BLE is connected to a client device
 * 
 * @return true if connected, false otherwise
 */
bool ble_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_H
