#include "board.h"
#include "display/display.h"
#include "config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <driver/i2c_master.h>
#include <string.h>
#include <string>
#include <math.h>
#include <time.h>     // 新增：用于获取时间、日期、星期
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "sd_card.h"
#include "./animation.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "codecs/es8311_audio_codec.h"

// 接收来自 UI 的外部状态
extern "C" char g_osd_time_str[16];
extern "C" bool g_is_video_playing;
extern "C" volatile bool g_is_on_screen1;

class WoodyDisplay;
static WoodyDisplay* g_woody_display = nullptr;

// 提前声明回调函数
static void animation_draw_cb(const uint16_t *bitmap, int w, int h);

#define TAG "WoodyBoard"

volatile int16_t g_touch_x = 0;
volatile int16_t g_touch_y = 0;
volatile bool g_touch_pressed = false;

static QueueHandle_t touch_evt_queue_ = NULL;

static void IRAM_ATTR touch_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(touch_evt_queue_, &gpio_num, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// ================= 1. 标准 8x16 数字/符号字库 =================
static const uint8_t osd_font_8x16[11][16] = {
    {0x00,0x00,0x00,0x1C,0x36,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x36,0x1C,0x00,0x00}, // 0
    {0x00,0x00,0x00,0x0C,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3F,0x00,0x00}, // 1
    {0x00,0x00,0x00,0x3E,0x63,0x63,0x63,0x03,0x06,0x0C,0x18,0x30,0x63,0x7F,0x00,0x00}, // 2
    {0x00,0x00,0x00,0x3E,0x63,0x63,0x03,0x06,0x1C,0x06,0x03,0x63,0x63,0x3E,0x00,0x00}, // 3
    {0x00,0x00,0x00,0x06,0x0E,0x0E,0x1E,0x36,0x36,0x66,0x7F,0x06,0x06,0x1F,0x00,0x00}, // 4
    {0x00,0x00,0x00,0x7F,0x60,0x60,0x60,0x7C,0x66,0x03,0x03,0x63,0x66,0x3C,0x00,0x00}, // 5
    {0x00,0x00,0x00,0x1C,0x36,0x60,0x60,0x7E,0x73,0x63,0x63,0x63,0x33,0x1E,0x00,0x00}, // 6
    {0x00,0x00,0x00,0x7F,0x63,0x06,0x06,0x0C,0x0C,0x18,0x18,0x18,0x18,0x18,0x00,0x00}, // 7
    {0x00,0x00,0x00,0x3E,0x63,0x63,0x63,0x36,0x1C,0x36,0x63,0x63,0x63,0x3E,0x00,0x00}, // 8
    {0x00,0x00,0x00,0x3C,0x66,0x63,0x63,0x63,0x67,0x3F,0x03,0x03,0x36,0x1C,0x00,0x00}, // 9
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00}  // :
};

// ================= 2. 标准 16x16 汉字字库 (宋体粗体) =================
// 索引：0:上 1:下 2:午 3:星 4:期 5:一 6:二 7:三 8:四 9:五 10:六 11:天 12:月 13:日
static const uint8_t osd_hanzi_16x16[14][32] = {
    {0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0xFC,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0xFF,0xFF,0x00,0x00}, // 0: 上
    {0x00,0x00,0xFF,0xFF,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x60,0x03,0x30,0x03,0x18,0x03,0x0C,0x03,0x0C,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00}, // 1: 下
    {0x0C,0x00,0x0C,0x00,0x0C,0x00,0x1F,0xFC,0x19,0x80,0x31,0x80,0x61,0x80,0x01,0x80,0x01,0x80,0xFF,0xFF,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80}, // 2: 午
    {0x00,0x00,0x1F,0xF8,0x18,0x18,0x1F,0xF8,0x18,0x18,0x1F,0xF8,0x01,0x80,0x19,0x80,0x1F,0xFC,0x31,0x80,0x61,0x80,0x1F,0xF8,0x01,0x80,0x01,0x80,0x7F,0xFE,0x00,0x00}, // 3: 星
    {0x33,0x00,0x33,0x7E,0x7F,0xE6,0x33,0x66,0x33,0x66,0x3F,0x7E,0x33,0x66,0x33,0x66,0x3F,0x66,0x33,0x7E,0x33,0x66,0xFF,0xE6,0x06,0xC6,0x33,0xC6,0x61,0x9E,0xC3,0x0C}, // 4: 期
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 5: 一
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00}, // 6: 二
    {0x00,0x00,0x00,0x00,0x7F,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00}, // 7: 三
    {0x00,0x00,0x00,0x00,0x7F,0xFE,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0x66,0x6C,0x3E,0x78,0x06,0x70,0x06,0x60,0x06,0x7F,0xFE,0x60,0x06,0x00,0x00}, // 8: 四
    {0x00,0x00,0x7F,0xFE,0x03,0x00,0x03,0x00,0x03,0x00,0x03,0x00,0x3F,0xF8,0x06,0x18,0x06,0x18,0x06,0x18,0x06,0x18,0x0C,0x18,0x0C,0x18,0x0C,0x18,0xFF,0xFF,0x00,0x00}, // 9: 五
    {0x03,0x00,0x01,0x80,0x00,0xC0,0x00,0xC0,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x06,0x60,0x06,0x30,0x0C,0x18,0x0C,0x0C,0x18,0x0C,0x30,0x06,0x60,0x06,0x00,0x00}, // 10:六
    {0x00,0x00,0x3F,0xFC,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0xFF,0xFF,0x01,0x80,0x03,0xC0,0x03,0xC0,0x06,0x60,0x06,0x60,0x0C,0x30,0x18,0x18,0x30,0x0C,0xE0,0x07}, // 11:天
    {0x00,0x00,0x1F,0xFC,0x18,0x0C,0x18,0x0C,0x18,0x0C,0x1F,0xFC,0x18,0x0C,0x18,0x0C,0x18,0x0C,0x1F,0xFC,0x18,0x0C,0x18,0x0C,0x30,0x0C,0x30,0x0C,0x60,0x3C,0xC0,0x18}, // 12:月
    {0x00,0x00,0x1F,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0xF8,0x18,0x18}  // 13:日
};

// ================= OSD 渲染引擎核心模块 =================

// 绘制单个 8x16 ASCII 字符 (数字和冒号)
static int draw_ascii_char(uint16_t *bitmap, int screen_w, char c, int x, int y, int scale) {
    int idx = -1;
    if (c >= '0' && c <= '9') idx = c - '0';
    else if (c == ':') idx = 10;
    else if (c == ' ') return 8 * scale; // 空格占位

    if (idx >= 0) {
        for (int row = 0; row < 16; row++) {
            uint8_t bits = osd_font_8x16[idx][row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1 << (7 - col))) {
                    for(int dy = 0; dy < scale; dy++) {
                        for(int dx = 0; dx < scale; dx++) {
                            int px = x + col * scale + dx;
                            int py = y + row * scale + dy;
                            // 黑色阴影描边，防止白色背景下看不清
                            if (px + 2 < screen_w && py + 2 < 360) bitmap[(py + 2) * screen_w + (px + 2)] = 0x0000; 
                            if (px < screen_w && py < 360) bitmap[py * screen_w + px] = 0xFFFF;
                        }
                    }
                }
            }
        }
    }
    return 8 * scale; // 返回字符占用的宽度，供光标累加
}

// 绘制单个 16x16 汉字字符
static int draw_hanzi_char(uint16_t *bitmap, int screen_w, int hanzi_idx, int x, int y, int scale) {
    if (hanzi_idx < 0 || hanzi_idx > 13) return 0;
    
    for (int row = 0; row < 16; row++) {
        // 读取 2 个字节拼成 16 位的点阵行数据 (大端序)
        uint16_t bits = (osd_hanzi_16x16[hanzi_idx][row * 2] << 8) | osd_hanzi_16x16[hanzi_idx][row * 2 + 1];
        
        for (int col = 0; col < 16; col++) {
            if (bits & (1 << (15 - col))) {
                for(int dy = 0; dy < scale; dy++) {
                    for(int dx = 0; dx < scale; dx++) {
                        int px = x + col * scale + dx;
                        int py = y + row * scale + dy;
                        if (px + 2 < screen_w && py + 2 < 360) bitmap[(py + 2) * screen_w + (px + 2)] = 0x0000;
                        if (px < screen_w && py < 360) bitmap[py * screen_w + px] = 0xFFFF;
                    }
                }
            }
        }
    }
    return 16 * scale; // 汉字天生比数字宽一倍
}

// ================= 排版与整体绘制逻辑 =================
static void draw_full_osd(uint16_t *bitmap, int screen_w) {
    // 实时获取系统时间
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);

    // 【1. 绘制时间 (HH:MM)】- 第一行
    char time_str[10];
    sprintf(time_str, "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
    
    int time_x = 20;    
    int time_y = 220;   // 【修改】整体大幅下移，压向屏幕底部区域
    int time_scale = 3; // 保持时间大小不变 (放大3倍，高度 48 像素)
    
    int cursor_x = time_x;
    for (int i=0; time_str[i] != '\0'; i++) {
        cursor_x += draw_ascii_char(bitmap, screen_w, time_str[i], cursor_x, time_y, time_scale);
    }

    // 【2. 绘制日期 (MM月DD日)】- 第二行
    int line2_y = time_y + 48 + 10; // 【排版】换行，与上一行留 10 像素间距 (y=278)
    int text_scale = 1;             // 【缩放】精准缩小至 1/3 (1倍大小，高度 16 像素)
    cursor_x = time_x;              // 左对齐

    int month = tm_info.tm_mon + 1;
    int day = tm_info.tm_mday;
    int wday = tm_info.tm_wday; // 0=周日，1=周一...

    // 绘制 月份
    if (month >= 10) cursor_x += draw_ascii_char(bitmap, screen_w, '0' + (month / 10), cursor_x, line2_y, text_scale);
    cursor_x += draw_ascii_char(bitmap, screen_w, '0' + (month % 10), cursor_x, line2_y, text_scale);
    cursor_x += draw_hanzi_char(bitmap, screen_w, 12, cursor_x, line2_y, text_scale); // 月
    
    // 绘制 日期
    if (day >= 10) cursor_x += draw_ascii_char(bitmap, screen_w, '0' + (day / 10), cursor_x, line2_y, text_scale);
    cursor_x += draw_ascii_char(bitmap, screen_w, '0' + (day % 10), cursor_x, line2_y, text_scale);
    cursor_x += draw_hanzi_char(bitmap, screen_w, 13, cursor_x, line2_y, text_scale); // 日


    // 【3. 绘制上午/下午 和 星期X】- 第三行
    int line3_y = line2_y + 16 + 5; // 【排版】再次换行，留 5 像素间距 (y=299)
    cursor_x = time_x;              // 左对齐

    // 绘制 上/下午
    if (tm_info.tm_hour < 12) {
        cursor_x += draw_hanzi_char(bitmap, screen_w, 0, cursor_x, line3_y, text_scale); // 上
    } else {
        cursor_x += draw_hanzi_char(bitmap, screen_w, 1, cursor_x, line3_y, text_scale); // 下
    }
    cursor_x += draw_hanzi_char(bitmap, screen_w, 2, cursor_x, line3_y, text_scale);     // 午

    cursor_x += 8 * text_scale; // 中间打一个空格，作为视觉隔断

    // 绘制 星期X
    cursor_x += draw_hanzi_char(bitmap, screen_w, 3, cursor_x, line3_y, text_scale); // 星
    cursor_x += draw_hanzi_char(bitmap, screen_w, 4, cursor_x, line3_y, text_scale); // 期
    
    int wday_idx = 11; // 默认：天(11)
    if (wday == 1) wday_idx = 5;      // 一
    else if (wday == 2) wday_idx = 6; // 二
    else if (wday == 3) wday_idx = 7; // 三
    else if (wday == 4) wday_idx = 8; // 四
    else if (wday == 5) wday_idx = 9; // 五
    else if (wday == 6) wday_idx = 10;// 六
    
    cursor_x += draw_hanzi_char(bitmap, screen_w, wday_idx, cursor_x, line3_y, text_scale);
}

// ================= 底层显示驱动与类定义 =================

class WoodyDisplay : public Display {
private:
    spi_device_handle_t spi_handle_;
    SemaphoreHandle_t lcd_mutex_ = nullptr;
    uint8_t *dma_buf_[2] = {nullptr, nullptr};
    size_t dma_buf_size_ = 0;
    i2c_master_dev_handle_t touch_handle_ = nullptr;
    esp_timer_handle_t fallback_timer_ = nullptr; 

    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}

    void send_cmd(uint8_t cmd) {
        gpio_set_level(GPIO_NUM_14, 0); 
        spi_transaction_t t;
        memset(&t, 0, sizeof(t)); 
        t.length = 8;
        t.tx_buffer = &cmd;
        spi_device_polling_transmit(spi_handle_, &t);
    }

    void send_data(const uint8_t *data, uint16_t len) {
        if (len == 0) return;
        gpio_set_level(GPIO_NUM_14, 1); 
        spi_transaction_t t;
        memset(&t, 0, sizeof(t)); 
        t.length = (size_t)len * 8;
        t.tx_buffer = data;
        spi_device_polling_transmit(spi_handle_, &t);
    }

    void write_cmd(uint8_t cmd) {
        gpio_set_level(GPIO_NUM_10, 0); 
        send_cmd(cmd);
        gpio_set_level(GPIO_NUM_10, 1); 
    }

    void write_cmd_data(uint8_t cmd, const uint8_t *data, uint16_t len) {
        gpio_set_level(GPIO_NUM_10, 0); 
        send_cmd(cmd);
        send_data(data, len);
        gpio_set_level(GPIO_NUM_10, 1); 
    }

    void lcd_init() {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << GPIO_NUM_21) | (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_10); 
        io_conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io_conf);
        gpio_set_level(GPIO_NUM_10, 1); 
        gpio_set_level(GPIO_NUM_21, 1); 

        gpio_set_level(GPIO_NUM_21, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(GPIO_NUM_21, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(GPIO_NUM_21, 1);
        vTaskDelay(pdMS_TO_TICKS(120));

        spi_device_interface_config_t devcfg = {};
        devcfg.clock_speed_hz = 80 * 1000 * 1000;
        devcfg.mode = 3;
        devcfg.spics_io_num = -1; 
        devcfg.queue_size = 7;
        ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &spi_handle_));

        const uint8_t d_bb[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5};
        write_cmd_data(0xBB, d_bb, sizeof(d_bb)); 

        const uint8_t d_a0[] = {0x00, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00};
        write_cmd_data(0xA0, d_a0, sizeof(d_a0));

        write_cmd(0x11); 
        vTaskDelay(pdMS_TO_TICKS(120));

        const uint8_t d_36[] = {0x00};
        write_cmd_data(0x36, d_36, 1); 

        const uint8_t d_3a[] = {0x55};
        write_cmd_data(0x3A, d_3a, 1); 

        const uint8_t d_35[] = {0x00};
        write_cmd_data(0x35, d_35, 1); 

        const uint8_t d_44[] = {0x01, 0x4A};
        write_cmd_data(0x44, d_44, 2); 

        write_cmd(0x29); 
        vTaskDelay(pdMS_TO_TICKS(20));

        const uint8_t d_raset[] = {0x00, 0x00, 0x01, 0x67}; 
        write_cmd_data(0x2B, d_raset, 4);

        const uint8_t d_caset[] = {0x00, 0x00, 0x01, 0x67};
        write_cmd_data(0x2A, d_caset, 4);

        write_cmd(0x2C); 
    }

    void draw_bitmap(const uint16_t *bitmap, uint16_t width, uint16_t height) {
        uint16_t x2 = width - 1;
        uint16_t y2 = height - 1;

        uint8_t raset[] = {0, 0, (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF)};
        write_cmd_data(0x2B, raset, 4);

        uint8_t caset[] = {0, 0, (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF)};
        write_cmd_data(0x2A, caset, 4);

        gpio_set_level(GPIO_NUM_10, 0);
        send_cmd(0x2C);

        const int chunk_lines = 15; 
        size_t line_width_bytes = width_ * 2;

        if (!dma_buf_[0] || !dma_buf_[1]) {
            gpio_set_level(GPIO_NUM_10, 1);
            return;
        }

        spi_transaction_t trans[2];
        memset(trans, 0, sizeof(trans));

        int buf_idx = 0;
        bool trans_active[2] = {false, false};

        gpio_set_level(GPIO_NUM_14, 1);

        for (int y = 0; y <= y2; y += chunk_lines) {
            int lines_to_copy = chunk_lines;
            if (y + lines_to_copy > y2 + 1) {
                lines_to_copy = (y2 + 1) - y;
            }
            size_t current_chunk_size = lines_to_copy * line_width_bytes;

            if (trans_active[buf_idx]) {
                spi_transaction_t *completed_trans;
                spi_device_get_trans_result(spi_handle_, &completed_trans, portMAX_DELAY);
                trans_active[buf_idx] = false;
            }

            memcpy(dma_buf_[buf_idx], (const uint8_t *)bitmap + y * width * 2, current_chunk_size);

            trans[buf_idx].length = current_chunk_size * 8;
            trans[buf_idx].tx_buffer = dma_buf_[buf_idx];
            
            spi_device_queue_trans(spi_handle_, &trans[buf_idx], portMAX_DELAY);
            trans_active[buf_idx] = true;

            buf_idx = 1 - buf_idx;
        }

        for (int i = 0; i < 2; i++) {
            if (trans_active[i]) {
                spi_transaction_t *completed_trans;
                spi_device_get_trans_result(spi_handle_, &completed_trans, portMAX_DELAY);
            }
        }

        gpio_set_level(GPIO_NUM_10, 1);
    }

    static void touch_poll_task(void* arg) {
        WoodyDisplay* display = (WoodyDisplay*)arg;
        uint32_t io_num;
        uint8_t read_buf[8];
        uint8_t read_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
        
        while (1) {
            if (xQueueReceive(touch_evt_queue_, &io_num, pdMS_TO_TICKS(50))) {
                if (display->touch_handle_) {
                    esp_err_t ret = i2c_master_transmit_receive(display->touch_handle_, read_cmd, sizeof(read_cmd), read_buf, 8, 50);
                    if (ret == ESP_OK) {
                        uint8_t points = read_buf[1]; 
                        int x = ((uint16_t)(read_buf[2] & 0x0F) << 8) | read_buf[3];
                        int y = ((uint16_t)(read_buf[4] & 0x0F) << 8) | read_buf[5];

                        if (points > 0 && points <= 5 && x < display->width_ && y < display->height_) {
                            g_touch_x = x;
                            g_touch_y = y;
                            g_touch_pressed = true;
                        } else {
                            g_touch_pressed = false;
                        }
                    }
                }
            } else {
                g_touch_pressed = false;
            }
        }
    }

    static void osd_fallback_timer_cb(void* arg) {
        WoodyDisplay* display = (WoodyDisplay*)arg;
        
        if (g_is_on_screen1 && !g_is_video_playing) {
            uint16_t* black_frame = (uint16_t*)malloc(360 * 360 * 2);
            if (black_frame) {
                memset(black_frame, 0, 360 * 360 * 2); 
                draw_full_osd(black_frame, 360); // 注入全新 OSD 排版
                display->draw_bitmap_safe(black_frame, 360, 360);
                free(black_frame);
            }
        }
    }

public:
    void draw_bitmap_safe(const uint16_t *bitmap, uint16_t width, uint16_t height) {
        if (lcd_mutex_ && xSemaphoreTake(lcd_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            draw_bitmap(bitmap, width, height);
            xSemaphoreGive(lcd_mutex_);
        }
    }

    void SetTouchDevice(i2c_master_dev_handle_t touch_handle) {
        touch_handle_ = touch_handle;
        xTaskCreate(touch_poll_task, "touch_poll", 4096, this, 15, NULL);
    }

    WoodyDisplay() {
        g_woody_display = this;
        lcd_mutex_ = xSemaphoreCreateMutex();
        
        width_ = 360;
        height_ = 360;
        
        dma_buf_size_ = 15 * width_ * 2; 
        dma_buf_[0] = (uint8_t *)heap_caps_malloc(dma_buf_size_, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        dma_buf_[1] = (uint8_t *)heap_caps_malloc(dma_buf_size_, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

        lcd_init();

        esp_timer_create_args_t timer_args = {};
        timer_args.callback = osd_fallback_timer_cb;
        timer_args.arg = this;
        timer_args.name = "osd_fallback";
        esp_timer_create(&timer_args, &fallback_timer_);
        esp_timer_start_periodic(fallback_timer_, 500000); 

        animation_init(animation_draw_cb);
        animation_enable_ble_init();
    }

    ~WoodyDisplay() {
        if (fallback_timer_) {
            esp_timer_stop(fallback_timer_);
            esp_timer_delete(fallback_timer_);
        }
        animation_deinit();
        if (dma_buf_[0]) free(dma_buf_[0]);
        if (dma_buf_[1]) free(dma_buf_[1]);
        if (lcd_mutex_) vSemaphoreDelete(lcd_mutex_);
    }

    virtual void SetEmotion(const char* emotion) override {}
    virtual void SetStatus(const char* status) override {}
    virtual void SetChatMessage(const char* role, const char* content) override {}
    virtual void ClearChatMessages() override {}
};

static void animation_draw_cb(const uint16_t *bitmap, int w, int h) {
    g_is_video_playing = true;

    if (!g_is_on_screen1) {
        animation_stop();
        return; 
    }

    if (g_woody_display) {
        uint16_t *mut_bitmap = (uint16_t *)bitmap;
        // 在视频画面上注入全新 OSD 排版
        draw_full_osd(mut_bitmap, w);
        g_woody_display->draw_bitmap_safe(mut_bitmap, w, h);
    }
}

class WoodyBoard : public Board {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Display* display_;
    AudioCodec* audio_codec_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {}; 
        i2c_bus_cfg.i2c_port = I2C_NUM_1;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 0;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 0;
        
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        
        gpio_config_t pa_conf = {};
        pa_conf.pin_bit_mask = (1ULL << GPIO_NUM_45);
        pa_conf.mode = GPIO_MODE_OUTPUT;
        pa_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        pa_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        pa_conf.intr_type = GPIO_INTR_DISABLE;
        
        gpio_config(&pa_conf);
        gpio_set_level(GPIO_NUM_45, 1); 
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_11;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_12;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeTouch() {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = 0x3B; 
        dev_cfg.scl_speed_hz = 400000;
        
        i2c_master_dev_handle_t touch_handle;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &touch_handle));

        gpio_config_t rst_io_conf = {};
        rst_io_conf.pin_bit_mask = (1ULL << GPIO_NUM_9);
        rst_io_conf.mode = GPIO_MODE_OUTPUT;
        rst_io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        rst_io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        rst_io_conf.intr_type = GPIO_INTR_DISABLE;
        
        gpio_config(&rst_io_conf);

        gpio_set_level(GPIO_NUM_9, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(GPIO_NUM_9, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(GPIO_NUM_9, 1);
        vTaskDelay(pdMS_TO_TICKS(100));

        if (touch_evt_queue_ == NULL) {
            touch_evt_queue_ = xQueueCreate(10, sizeof(uint32_t));
        }
        
        gpio_config_t int_io_conf = {};
        int_io_conf.pin_bit_mask = (1ULL << GPIO_NUM_46);
        int_io_conf.mode = GPIO_MODE_INPUT;
        int_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        int_io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        int_io_conf.intr_type = GPIO_INTR_NEGEDGE; 
        
        gpio_config(&int_io_conf);
        
        gpio_install_isr_service(0);
        gpio_isr_handler_add(GPIO_NUM_46, touch_isr_handler, (void*)GPIO_NUM_46);

        static_cast<WoodyDisplay*>(display_)->SetTouchDevice(touch_handle);
    }

public:
    WoodyBoard() {
        InitializeI2c();

        static Es8311AudioCodec es8311(
            i2c_bus_,
            I2C_NUM_1,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_45, 
            AUDIO_CODEC_ES8311_ADDR
        );
        audio_codec_ = &es8311;

        InitializeSpi();
        display_ = new WoodyDisplay();
        InitializeTouch();
        GetBacklight()->RestoreBrightness();
        StartNetwork();
    }
    
    virtual AudioCodec* GetAudioCodec() override { return audio_codec_; }
    virtual std::string GetBoardType() override { return "WoodyBoard"; }
    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual NetworkInterface* GetNetwork() override { return nullptr; }
    virtual void StartNetwork() override {
        ESP_LOGI(TAG, "Starting Wi-Fi and SNTP for time sync...");

        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }

        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_wifi_set_mode(WIFI_MODE_STA);

        wifi_config_t wifi_config = {};
        strcpy((char*)wifi_config.sta.ssid, "H3C_AP_2.4");
        strcpy((char*)wifi_config.sta.password, "10mucdrom");

        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_start();
        esp_wifi_connect(); 

        setenv("TZ", "CST-8", 1);
        tzset();

        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "ntp.aliyun.com");
        sntp_setservername(1, "cn.pool.ntp.org");
        sntp_init();

        StackType_t *ble_delay_stack = (StackType_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        StaticTask_t *ble_delay_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        
        if (ble_delay_stack && ble_delay_tcb) {
            xTaskCreateStaticPinnedToCore([](void* arg) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                animation_start_ble(); 
                vTaskSuspend(NULL); 
            }, "delayed_ble", 8192, NULL, 5, ble_delay_stack, ble_delay_tcb, tskNO_AFFINITY);
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for delayed_ble task!");
        }
    }
    virtual const char* GetNetworkStateIcon() override { return ""; }
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {}
    virtual std::string GetBoardJson() override { return "{}"; }
    virtual std::string GetDeviceStatusJson() override { return "{}"; }
};

DECLARE_BOARD(WoodyBoard);
extern "C" void push_pixels_to_screen(const uint16_t *bitmap, int w, int h) {
    if (g_woody_display) {
        g_woody_display->draw_bitmap_safe(bitmap, w, h);
    }
}