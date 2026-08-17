#include "audio_player.h"       // <--- 【核心修复】必须包含自己的类定义头文件！
#include "audio_player_c_api.h" // 包含我们刚刚写的纯 C 接口
#include <dirent.h>
#include "esp_log.h"
#include <string.h>

static const char* TAG = "AudioPlayer";

// 引用我们在 mp3_decoder.cc 里写的解码函数
extern void PlayLocalMP3(const char* filepath, volatile bool* stop_flag);

void AudioPlayer::LoadMusicDirectory(const char* dir_path) {
    playlist_.clear();
    DIR* dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "无法打开目录: %s", dir_path);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) { 
            std::string filename = entry->d_name;
            if (filename.length() >= 4) {
                std::string ext = filename.substr(filename.length() - 4);
                // 简单转小写比较，或直接匹配 .mp3 / .MP3
                if (ext == ".mp3" || ext == ".MP3") {
                    playlist_.push_back(std::string(dir_path) + "/" + filename);
                    ESP_LOGI(TAG, "加入播放列表: %s", filename.c_str());
                }
            }
        }
    }
    closedir(dir);
    current_index_ = 0;
    ESP_LOGI(TAG, "共加载了 %d 首歌曲", playlist_.size());
}

void AudioPlayer::Play() {
    if (playlist_.empty()) {
        ESP_LOGW(TAG, "播放列表为空");
        return;
    }

    // 如果当前正在播放，先停止旧任务
    Stop();

    stop_flag_ = false;
    is_playing_ = true;

    // 创建一个独立的 FreeRTOS 任务来执行解码，防止阻塞 UI 线程
    // 注意：解码需要较大的栈空间，这里分配了 8192 字节 (8KB)
    xTaskCreate(PlayTaskWrapper, "mp3_play_task", 8192, this, 5, &play_task_handle_);
}

void AudioPlayer::Stop() {
    if (is_playing_ && play_task_handle_ != nullptr) {
        stop_flag_ = true; // 通知解码循环退出
        // 等待一小会儿让解码器安全关闭文件
        vTaskDelay(pdMS_TO_TICKS(50)); 
        play_task_handle_ = nullptr;
    }
    is_playing_ = false;
}

void AudioPlayer::PlayNext() {
    if (playlist_.empty()) return;
    current_index_ = (current_index_ + 1) % playlist_.size();
    Play();
}

void AudioPlayer::PlayPrev() {
    if (playlist_.empty()) return;
    current_index_ = (current_index_ - 1 + playlist_.size()) % playlist_.size();
    Play();
}

std::string AudioPlayer::GetCurrentSongName() {
    if (playlist_.empty()) return "无音乐";
    // 从完整路径中提取文件名
    std::string path = playlist_[current_index_];
    size_t pos = path.find_last_of("/");
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

// 这是跑在独立线程里的实际播放逻辑
void AudioPlayer::PlayTaskWrapper(void* arg) {
    AudioPlayer* player = static_cast<AudioPlayer*>(arg);
    
    std::string current_file = player->playlist_[player->current_index_];
    ESP_LOGI(TAG, "后台任务开始播放: %s", current_file.c_str());

    // 调用 mp3_decoder.cc 里的解码逻辑，并将 stop_flag_ 的指针传进去
    PlayLocalMP3(current_file.c_str(), &player->stop_flag_);

    ESP_LOGI(TAG, "单曲播放完毕");
    player->is_playing_ = false;
    player->play_task_handle_ = nullptr;

    // TODO: 如果你想实现“自动播放下一首”，可以取消下面这行的注释
    // player->PlayNext(); 

    vTaskDelete(NULL); // 自我销毁任务
}

extern "C" {

    void audio_play_c_load_dir(const char* dir_path) {
        AudioPlayer::GetInstance().LoadMusicDirectory(dir_path);
    }

    void audio_play_c_play(void) {
        AudioPlayer::GetInstance().Play();
    }

    void audio_play_c_pause(void) {
        AudioPlayer::GetInstance().Pause();
    }

    void audio_play_c_stop(void) {
        AudioPlayer::GetInstance().Stop();
    }

    void audio_play_c_next(void) {
        AudioPlayer::GetInstance().PlayNext();
    }

    void audio_play_c_prev(void) {
        AudioPlayer::GetInstance().PlayPrev();
    }

    bool audio_play_c_is_playing(void) {
        return AudioPlayer::GetInstance().IsPlaying();
    }

    void audio_play_c_get_song_name(char* buffer, int max_len) {
        if (!buffer || max_len <= 0) return;
        std::string name = AudioPlayer::GetInstance().GetCurrentSongName();
        strncpy(buffer, name.c_str(), max_len - 1);
        buffer[max_len - 1] = '\0'; // 确保字符串正确结束
    }

}