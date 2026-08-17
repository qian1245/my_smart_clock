#include "audio_service.h"
#include <esp_log.h>

// 假设原生的 audio_service.cc 中有获取单例或全局服务的函数
// 如果没有，可以根据你的 audio_service.cc 实际情况获取实例
extern AudioService& GetAudioService(); 

extern "C" {

void audio_play_music_bridge(const char* file_path) {
    ESP_LOGI("AudioBridge", "Bridge playing: %s", file_path);
    // 直接调用我们刚才在 audio_service.cc 里加的方法
    GetAudioService().PlayLocalFile(file_path);
}

void audio_stop_music_bridge(void) {
    ESP_LOGI("AudioBridge", "Bridge stopping music");
    // 如果需要停止，可以在这里扩展
}

}