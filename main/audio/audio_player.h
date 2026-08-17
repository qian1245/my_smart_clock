#pragma once

#include <vector>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class AudioPlayer {
public:
    // 获取单例实例
    static AudioPlayer& GetInstance() {
        static AudioPlayer instance;
        return instance;
    }

    // 扫描目录并加载播放列表
    void LoadMusicDirectory(const char* dir_path);

    // 播放控制
    void Play();
    void Pause(); // 暂停/恢复
    void Stop();
    void PlayNext();
    void PlayPrev();

    // 状态查询 (供 UI 更新界面)
    bool IsPlaying() const { return is_playing_; }
    std::string GetCurrentSongName();

    // 暴露给解码器的停止标志位
    bool check_stop_flag() const { return stop_flag_; }

private:
    AudioPlayer() = default;
    ~AudioPlayer() = default;

    std::vector<std::string> playlist_;
    int current_index_ = 0;
    
    bool is_playing_ = false;
    volatile bool stop_flag_ = false; // 用于通知后台解码任务退出

    TaskHandle_t play_task_handle_ = nullptr;

    // 后台播放任务
    static void PlayTaskWrapper(void* arg);
};