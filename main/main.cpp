#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "board.h"
#include "display.h"   
#include "lvgl.h"
#include "ui/ui.h" 
#include "nvs_flash.h"

static const char *TAG = "Main";

// 声明底层的画图接口
extern "C" void push_pixels_to_screen(const uint16_t *bitmap, int w, int h);

// 声明 ui_custom.c 里的自定义逻辑初始化
extern "C" void ui_custom_logic_init(void);

// 声明触摸变量（由底层板卡驱动更新）
extern volatile int16_t g_touch_x;
extern volatile int16_t g_touch_y;
extern volatile bool g_touch_pressed;

// LVGL 刷屏回调
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;

    uint16_t *buf16 = (uint16_t *)px_map;
    uint32_t px_cnt = width * height;
    for(uint32_t i = 0; i < px_cnt; i++) {
        buf16[i] = (buf16[i] >> 8) | (buf16[i] << 8);
    }

    // 调用底层暴露的 C 接口推给屏幕
    push_pixels_to_screen(buf16, width, height);
    
    // 必须通知 LVGL 刷新完成
    lv_display_flush_ready(disp);
}

// 触摸读取回调
static void my_touch_read(lv_indev_t * indev, lv_indev_data_t * data) {
    if(g_touch_pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = g_touch_x;
        data->point.y = g_touch_y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lv_tick_task(void *arg) {
    lv_tick_inc(2);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Smart Clock Starting ===");

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化主板硬件与蓝牙/外设
    Board::GetInstance();

    // 3. 初始化 LVGL
    lv_init();

    // --- 4. 注册屏幕 ---
    lv_display_t *disp = lv_display_create(360, 360);
    uint32_t buf_size = 360 * 360 * 2;
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf1, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, my_disp_flush);

    // --- 5. 注册触摸屏 ---
     lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_read);
    lv_indev_set_display(indev, disp);  // 【新增】将触摸设备绑定到显示设备
    // lv_indev_enable(indev);  // 无需调用，lv_indev_create() 默认启用

    // --- 6. 启动 LVGL 定时器 ---
    esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &lv_tick_task;
    lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
    esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, 2000);

   // --- 7. 初始化 UI ---
    ui_init();                  
    ui_custom_logic_init();     

    ESP_LOGI(TAG, "UI initialized successfully! Entering LVGL main loop...");

    // 【核心修复】：将当前 app_main 任务的优先级从默认的 1 提升到 10！
    // 这样 LVGL 的刷新和手势处理优先级就会高于视频播放任务（优先级5）
    vTaskPrioritySet(NULL, 10);

    while(1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}