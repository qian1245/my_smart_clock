#include "board.h"
#include "display/display.h"
//#include "display/oled_display.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_chip_info.h>
#include <esp_random.h>

#define TAG "Board"

Board::Board() {
    // [已阉割] 因为去掉了 settings，这部分 UUID 读取和保存的逻辑全部废弃
    // Settings settings("board", true);
    // uuid_ = settings.GetString("uuid");
    // if (uuid_.empty()) {
    //     uuid_ = GenerateUuid();
    //     settings.SetString("uuid", uuid_);
    // }
    
    // 随便给个默认 UUID 即可，防止其他地方用到崩溃
    uuid_ = "12345678-1234-1234-1234-123456789abc"; 
   ESP_LOGI(TAG, "UUID=%s SKU=Woody", uuid_.c_str());
}

std::string Board::GenerateUuid() {
    // 这个函数本身没有依赖报错的头文件，保留它以防 board.h 里有声明导致编译报错
    uint8_t uuid[16];
    
    esp_fill_random(uuid, sizeof(uuid));
    
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    
    char uuid_str[37];
    snprintf(uuid_str, sizeof(uuid_str),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5], uuid[6], uuid[7],
        uuid[8], uuid[9], uuid[10], uuid[11],
        uuid[12], uuid[13], uuid[14], uuid[15]);
    
    return std::string(uuid_str);
}

bool Board::GetBatteryLevel(int &level, bool& charging, bool& discharging) {
    return false;
}

bool Board::GetTemperature(float& esp32temp){
    return false;
}

Display* Board::GetDisplay() {
    static NoDisplay display;
    return &display;
}

Camera* Board::GetCamera() {
    return nullptr;
}

Led* Board::GetLed() {
    static NoLed led;
    return &led;
}

std::string Board::GetSystemInfoJson() {
    // [已阉割] 这里原本有上百行代码，高度依赖 SystemInfo 和 Lang，用于给配网 APP 发送设备状态。
    // 我们的纯净版闹钟完全不需要这些，直接返回空 JSON，彻底切断报错源头！
    return "{}"; 
}