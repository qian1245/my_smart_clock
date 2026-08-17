// 必须定义此宏，才能真正编译出 minimp3 的核心函数实现
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <stdio.h>
#include <string.h>
#include <vector>  // 添加 vector 头文件
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 引入你的项目依赖头文件
#include "boards/common/board.h"
#include "audio_codec.h" 

static const char* TAG = "MP3_DECODER";

void PlayLocalMP3(const char* filepath, volatile bool* stop_flag) {
    // 1. 获取底层的音频编解码器实例
    // 注意：如果你的 Board 类中获取音频实例的方法不叫 GetAudioCodec()，请在此修改
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "获取底层 AudioCodec 失败，无法播放！");
        return;
    }

    // ================== 【核心修复：唤醒并解除静音】 ==================
    ESP_LOGI(TAG, "正在唤醒 ES8311 芯片并打开功放...");
    codec->Start();                // 启动底层的 I2S 和 I2C 传输
    codec->SetOutputVolume(90);    // 设置音量，范围一般是 0 - 100，这里设为 90
    codec->EnableOutput(true);     // 关键！真正打开 DAC 输出和硬件功放 (PA) 引脚
    // =================================================================
    
    // 2. 打开 SD 卡上的 MP3 文件
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "无法打开 MP3 文件: %s", filepath);
        return;
    }

    // 3. 初始化解码器
    mp3dec_t mp3d;
    mp3dec_init(&mp3d);
    mp3dec_frame_info_t info;

    // ESP32 内存有限，分配在栈上的 buffer 需谨慎（这里分配了 4KB 读缓存）
    unsigned char input_buf[4096];
    short pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
    
    int buf_size = 0;
    bool is_eof = false;
    bool is_first_frame = true;

    ESP_LOGI(TAG, "开始播放本地 MP3: %s", filepath);

    // 4. 进入边读边解边播循环
    while (!is_eof || buf_size > 0) {
        // 【新增这几行】：优先检查是否收到来自 UI 的停止/切歌指令
        if (stop_flag != nullptr && *stop_flag == true) {
            ESP_LOGI(TAG, "收到停止/切歌指令，立即中断解码...");
            break; // 直接跳出 while 循环，进入后面的 fclose(file) 关闭文件
        }

        // 补充输入缓冲区 (你原本的代码)
        if (!is_eof && buf_size < sizeof(input_buf)) {
            size_t bytes_read = fread(input_buf + buf_size, 1, sizeof(input_buf) - buf_size, file);
            buf_size += bytes_read;
            if (bytes_read == 0) {
                is_eof = true; // 读到文件末尾
            }
        }

        // 解码一帧数据
        int samples = mp3dec_decode_frame(&mp3d, input_buf, buf_size, pcm_buf, &info);

        if (samples > 0) {
            // 第一帧解码成功后，动态配置底层 I2S/DAC 的参数
            if (is_first_frame) {
                ESP_LOGI(TAG, "MP3 音频参数: 采样率 %d Hz, 声道 %d, 比特率 %d kbps", info.hz, info.channels, info.bitrate_kbps);
                is_first_frame = false;
            }

            // 计算该帧解码出的采样数
            size_t pcm_samples = samples * info.channels;
            
            // 将 PCM 数据放入 vector 中，然后通过 OutputData 播放
            std::vector<int16_t> pcm_data(pcm_buf, pcm_buf + pcm_samples);
            codec->OutputData(pcm_data);
        }

        // 剔除已消耗的 MP3 压缩数据，移动缓冲区指针
        if (info.frame_bytes > 0) {
            buf_size -= info.frame_bytes;
            memmove(input_buf, input_buf + info.frame_bytes, buf_size);
        } else if (samples == 0 && buf_size > 0 && is_eof) {
            break; // 文件尾部存在无用数据（如 ID3 标签尾部），跳出循环
        }
        
        // 喂狗：交出 2ms 的 CPU 时间，防止解码占用太久触发 Watchdog 复位
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    fclose(file);
    ESP_LOGI(TAG, "MP3 播放结束");
}
