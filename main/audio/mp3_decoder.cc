// 必须定义此宏，才能真正编译出 minimp3 的核心函数实现
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boards/common/board.h"
#include "audio_codec.h" 

static const char* TAG = "MP3_DECODER";

// 定义缓冲区大小常量
#define MP3_INPUT_BUF_SIZE 4096

void PlayLocalMP3(const char* filepath, volatile bool* stop_flag) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "获取底层 AudioCodec 失败，无法播放！");
        return;
    }

    ESP_LOGI(TAG, "正在唤醒 ES8311 芯片并打开功放...");
    codec->Start();                
    codec->SetOutputVolume(90);    
    codec->EnableOutput(true);     
    
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "无法打开 MP3 文件: %s", filepath);
        return;
    }

    mp3dec_t mp3d;
    mp3dec_init(&mp3d);
    mp3dec_frame_info_t info;

    // 【修改1】使用堆内存动态分配，防止栈溢出
    unsigned char* input_buf = (unsigned char*)malloc(MP3_INPUT_BUF_SIZE);
    short* pcm_buf = (short*)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(short));
    
    if (input_buf == nullptr || pcm_buf == nullptr) {
        ESP_LOGE(TAG, "内存不足，无法为MP3解码器分配缓冲区！");
        if(input_buf) free(input_buf);
        if(pcm_buf) free(pcm_buf);
        fclose(file);
        return;
    }

    // 【修改2】把 vector 定义在循环外部，避免频繁分配内存
    std::vector<int16_t> pcm_data;
    pcm_data.reserve(MINIMP3_MAX_SAMPLES_PER_FRAME); 
    
    int buf_size = 0;
    bool is_eof = false;
    bool is_first_frame = true;

    ESP_LOGI(TAG, "开始播放本地 MP3: %s", filepath);

    while (!is_eof || buf_size > 0) {
        // 优先检查停止指令
        if (stop_flag != nullptr && *stop_flag == true) {
            ESP_LOGI(TAG, "收到停止/切歌指令，立即中断解码...");
            break; 
        }

        // 补充输入缓冲区（注意把 sizeof 换成宏）
        if (!is_eof && buf_size < MP3_INPUT_BUF_SIZE) {
            size_t bytes_read = fread(input_buf + buf_size, 1, MP3_INPUT_BUF_SIZE - buf_size, file);
            buf_size += bytes_read;
            if (bytes_read == 0) {
                is_eof = true; 
            }
        }

        int samples = mp3dec_decode_frame(&mp3d, input_buf, buf_size, pcm_buf, &info);

        if (samples > 0) {
            if (is_first_frame) {
                ESP_LOGI(TAG, "MP3 音频参数: %d Hz, 声道 %d, 比特率 %d kbps", info.hz, info.channels, info.bitrate_kbps);
                
                // 【修改3】同步更新底层硬件的采样率和声道参数
                // 注意：请根据你的 codec 类的实际方法名进行调用
                // codec->SetSampleRate(info.hz);
                // codec->SetChannels(info.channels);
                
                is_first_frame = false;
            }

            size_t pcm_samples = samples * info.channels;
            
            // 【修改2】复用外部创建的 vector
            pcm_data.assign(pcm_buf, pcm_buf + pcm_samples);
            codec->OutputData(pcm_data);
            
            // 进阶优化提示：如果你的 codec->OutputData() 允许传指针，直接使用下面这种方式性能最好，完全省去 std::vector 开销：
            // codec->OutputData((uint8_t*)pcm_buf, pcm_samples * sizeof(short));
        }

        // 移动缓冲区指针
        if (info.frame_bytes > 0) {
            buf_size -= info.frame_bytes;
            memmove(input_buf, input_buf + info.frame_bytes, buf_size);
        } else if (samples == 0 && buf_size > 0 && is_eof) {
            break; 
        }
        
        // 【修改4】去掉强制 delay。I2S 写入(OutputData)通常会自行阻塞。
        // 如果你的看门狗触发了，说明底层不是阻塞写入，可以用 taskYIELD() 替代
        // taskYIELD(); 
    }

    // 务必释放动态分配的内存
    free(input_buf);
    free(pcm_buf);
    fclose(file);
    ESP_LOGI(TAG, "MP3 播放结束");
}