#include "display.h"
#include <esp_log.h>

#define TAG "Display"

Display::Display() {
}

Display::~Display() {
}

void Display::SetStatus(const char* status) {
    if (status) {
        ESP_LOGI(TAG, "Status: %s", status);
    }
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    if (notification) {
        ESP_LOGI(TAG, "Notification: %s", notification);
    }
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ESP_LOGI(TAG, "Notification: %s", notification.c_str());
}

void Display::SetEmotion(const char* emotion) {
}

void Display::SetChatMessage(const char* role, const char* content) {
}

void Display::ClearChatMessages() {
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
}

void Display::UpdateStatusBar(bool update_all) {
}

void Display::SetPowerSaveMode(bool on) {
}