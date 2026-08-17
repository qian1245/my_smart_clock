#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ui_custom.h"
#include "esp_heap_caps.h"
#include "audio_player_c_api.h"

// ================= 声明中文字体 =================
LV_FONT_DECLARE(my_font_chinese_24);

// ================= 外部动画接口 =================
extern void animation_start(void);
extern void animation_stop(void);
extern int animation_get_total_sequences(void);
extern uint16_t* animation_get_preview_frame(int seq_idx);
extern void animation_set_active_sequence(int seq_idx);
extern int animation_get_active_sequence(void);

// 声明一个纯 C 的桥接函数接口
extern void audio_play_music_bridge(const char* file_path);
extern void audio_stop_music_bridge(void);

void audio_play_music(const char* file_path) {
    audio_play_music_bridge(file_path);
}

void audio_stop_music(void) {
    audio_stop_music_bridge();
}

// ================= 音乐与SD卡相关变量 =================
#define MAX_MUSIC_FILES 20
#define MAX_MUSIC_NAME_LEN 64
static char music_list[MAX_MUSIC_FILES][MAX_MUSIC_NAME_LEN];
static int total_musics = 0;
int current_music_index = 0;
const char* MUSIC_DIR = "/sdcard/music";

// ================= 闹钟与其他变量 =================
int current_video_index = 0;

typedef struct {
    int hour;
    int minute;
    int second;
    bool is_active;
} AlarmRecord;

AlarmRecord alarms[3] = {0};
int alarm_count = 0;
int current_ringing_alarm_idx = -1;

char g_osd_time_str[16] = "00:00:00";
bool g_is_video_playing = false; 
volatile bool g_is_on_screen1 = true;

// ===== Screen2 动态背景相关变量 =====
static int current_preview_seq_index = 0;

#if defined(LV_IMG_CF_TRUE_COLOR)
static lv_img_dsc_t dynamic_bg_dsc = {
    .header = { .always_zero = 0, .w = 360, .h = 360, .cf = LV_IMG_CF_TRUE_COLOR },
    .data_size = 360 * 360 * 2, .data = NULL
};
#else
#ifndef LV_IMAGE_HEADER_MAGIC
#define LV_IMAGE_HEADER_MAGIC 0x19
#endif
static lv_image_dsc_t dynamic_bg_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .flags = 0, .w = 360, .h = 360, .stride = 360 * 2 },
    .data_size = 360 * 360 * 2, .data = NULL
};
#endif

static lv_obj_t * dynamic_bg_obj = NULL;

// ================= 新增：壁纸预览刷新函数 =================
static void update_wallpaper_preview(void) {
    if (current_preview_seq_index == 0) {
        if (dynamic_bg_obj) {
            lv_obj_del(dynamic_bg_obj);
            dynamic_bg_obj = NULL;
        }
        if (dynamic_bg_dsc.data) {
            heap_caps_free((void*)dynamic_bg_dsc.data);
            dynamic_bg_dsc.data = NULL;
        }
    } else {
        uint16_t* preview_buf = animation_get_preview_frame(current_preview_seq_index);
        if (preview_buf) {
            if (dynamic_bg_dsc.data) {
                heap_caps_free((void*)dynamic_bg_dsc.data);
            }
            dynamic_bg_dsc.data = (const uint8_t *)preview_buf;
            
            if (!dynamic_bg_obj) {
                dynamic_bg_obj = lv_img_create(ui_Screen2);
                lv_obj_move_background(dynamic_bg_obj);
            }
            lv_img_set_src(dynamic_bg_obj, &dynamic_bg_dsc);
            lv_obj_invalidate(dynamic_bg_obj);
        }
    }
}

// ================= 扫描 SD 卡音乐文件 =================
static void scan_music_files(void) {
    total_musics = 0;
    struct stat st;
    if (stat(MUSIC_DIR, &st) != 0) {
        mkdir(MUSIC_DIR, 0777);
        strcpy(music_list[0], "无音乐文件");
        total_musics = 1;
        return;
    }

    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) {
        strcpy(music_list[0], "无音乐文件");
        total_musics = 1;
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && total_musics < MAX_MUSIC_FILES) {
        if (strstr(ent->d_name, ".mp3") || strstr(ent->d_name, ".wav")) {
            strlcpy(music_list[total_musics], ent->d_name, MAX_MUSIC_NAME_LEN);
            total_musics++; 
        }
    }
    closedir(dir);

    if (total_musics == 0) {
        strcpy(music_list[0], "无音乐文件");
        total_musics = 1;
    }
}

// ================= Button/Roller 透明设置 =================
static void button_set_transparent(lv_obj_t * btn) {
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
}

static void roller_set_transparent(lv_obj_t * roller) {
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_SELECTED);
}

// ================= 设置其余 Label 的中文字体 =================
static void set_chinese_font(void) {
    lv_obj_set_style_text_font(ui_Label6, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label13, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label16, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label17, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label18, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label7, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label8, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label9, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label10, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label11, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label12, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label14, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label15, &my_font_chinese_24, 0);

    if (lv_obj_is_valid(ui_Label5)) {
        lv_obj_set_style_text_font(ui_Label5, &my_font_chinese_24, 0);
        lv_obj_set_height(ui_Label5, LV_SIZE_CONTENT);
    }
    if (lv_obj_is_valid(ui_Label19)) {
        lv_obj_set_style_text_font(ui_Label19, &my_font_chinese_24, 0);
        lv_obj_set_height(ui_Label19, LV_SIZE_CONTENT);
    }
    if (lv_obj_is_valid(ui_Label20)) {
        lv_obj_set_style_text_font(ui_Label20, &my_font_chinese_24, 0);
        lv_obj_set_height(ui_Label20, LV_SIZE_CONTENT);
    }
    if (lv_obj_is_valid(ui_Label1)) {
        lv_obj_set_style_text_font(ui_Label1, &my_font_chinese_24, 0);
        lv_obj_set_height(ui_Label1, LV_SIZE_CONTENT);
    }
}

// ================= 新增：延时安全播放回调 =================
static void delayed_audio_play_cb(lv_timer_t * timer) {
    audio_play_c_play();
    lv_timer_del(timer); // 触发一次后自动销毁定时器
}

// ================= 时钟定时器 =================
static void clock_timer_cb(lv_timer_t * timer) {
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    char time_str[32];
    sprintf(time_str, "%02d:%02d", tm.tm_hour, tm.tm_min);
    
    lv_obj_t * current_scr = lv_disp_get_scr_act(NULL);

    if (current_scr == ui_Screen1) {
        strcpy(g_osd_time_str, time_str);
    } else {
        g_osd_time_str[0] = '\0';
    }

    for (int i = 0; i < 3; i++) {
        if (alarms[i].is_active && 
            alarms[i].hour == tm.tm_hour && 
            alarms[i].minute == tm.tm_min && 
            alarms[i].second == tm.tm_sec) {
            
            alarms[i].is_active = false; 
            
            switch(i) {
                case 0: lv_label_set_text(ui_Label12, "未启用"); break;
                case 1: lv_label_set_text(ui_Label11, "未启用"); break;
                case 2: lv_label_set_text(ui_Label10, "未启用"); break;
            }

            current_ringing_alarm_idx = i;
            
            if (lv_obj_is_valid(ui_Label20)) {
                char ring_time[16];
                sprintf(ring_time, "%02d:%02d", alarms[i].hour, alarms[i].minute);
                lv_label_set_text(ui_Label20, ring_time);
            }
            
            if(ui_Screen5 != NULL) {
                if (g_is_on_screen1) {
                    g_is_on_screen1 = false;
                    animation_stop(); 
                }
                
                lv_disp_load_scr(ui_Screen5);
                lv_timer_create(delayed_audio_play_cb, 600, NULL);
            }
        } 
    }
}

// ================= Screen1 & Screen2 事件 =================
static void screen1_bg_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_LONG_PRESSED) {
        g_is_on_screen1 = false; 
        animation_stop();

        current_preview_seq_index = animation_get_active_sequence();
        update_wallpaper_preview();

        lv_disp_load_scr(ui_Screen2);
    }
    else if(code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if(dir == LV_DIR_LEFT) {
            g_is_on_screen1 = false; 
            animation_stop();
            lv_disp_load_scr(ui_Screen4);
        } 
    }
}

static void screen2_next_btn_cb(lv_event_t * e) {
    int total = animation_get_total_sequences();
    if (total <= 0) total = 1;
    current_preview_seq_index = (current_preview_seq_index + 1) % total;
    update_wallpaper_preview();
}

static void screen2_apply_btn_cb(lv_event_t * e) {
    animation_set_active_sequence(current_preview_seq_index);

    // 【修复1】：应用背景时不直接删除，仅解绑，避免野指针崩溃
    if (dynamic_bg_obj) {
        lv_img_set_src(dynamic_bg_obj, NULL);
    }
    
    if (dynamic_bg_dsc.data) {
        heap_caps_free((void*)dynamic_bg_dsc.data);
        dynamic_bg_dsc.data = NULL;
    }
    
    g_is_on_screen1 = true; 
    lv_disp_load_scr(ui_Screen1);

    if (current_preview_seq_index != 0) {
        animation_start();
    }
}

// ================= Screen3 事件：切换音乐 =================
static void screen3_prev_track_cb(lv_event_t * e) {
    audio_play_c_prev();
    if (lv_obj_is_valid(ui_Label17)) {
        char song_name[128];
        audio_play_c_get_song_name(song_name, sizeof(song_name));
        lv_label_set_text(ui_Label17, song_name);
    }
}

static void screen3_next_track_cb(lv_event_t * e) {
    audio_play_c_next();
    if (lv_obj_is_valid(ui_Label17)) {
        char song_name[128];
        audio_play_c_get_song_name(song_name, sizeof(song_name));
        lv_label_set_text(ui_Label17, song_name);
    }
}

static void screen3_apply_btn_cb(lv_event_t * e) {
    audio_play_c_stop();
    // 【修复3】：强制 UI 挂起 100ms，等待音频任务释放底层资源
    vTaskDelay(pdMS_TO_TICKS(100)); 
    
    lv_disp_load_scr(ui_Screen4);
}

// ================= Screen4 事件 =================
static void screen4_apply_btn_cb(lv_event_t * e) {
    int h = lv_roller_get_selected(ui_Roller1);
    int m = lv_roller_get_selected(ui_Roller2);
    int s = lv_roller_get_selected(ui_Roller3);
    
    int idx = alarm_count % 3;
    alarms[idx].hour = h;
    alarms[idx].minute = m;
    alarms[idx].second = s;
    alarms[idx].is_active = true;
    
    char buf[32];
    sprintf(buf, "%02d:%02d:%02d", h, m, s);
    
    switch(idx) {
        case 0: lv_label_set_text(ui_Label7, buf);  lv_label_set_text(ui_Label12, "已启用"); break;
        case 1: lv_label_set_text(ui_Label8, buf);  lv_label_set_text(ui_Label11, "已启用"); break;
        case 2: lv_label_set_text(ui_Label9, buf);  lv_label_set_text(ui_Label10, "已启用"); break;
    }
    
    alarm_count++;
}

static void screen4_custom_btn_cb(lv_event_t * e) {
    scan_music_files();
    
    if (current_music_index >= total_musics) {
        current_music_index = 0;
    }
    
    if(lv_obj_is_valid(ui_Label17)) {
        char song_name[128];
        audio_play_c_get_song_name(song_name, sizeof(song_name));
        lv_label_set_text(ui_Label17, song_name);
    }
    
    audio_play_c_play();
    
    lv_disp_load_scr(ui_Screen3);
}

static void screen4_toggle_alarm_0_cb(lv_event_t * e) {
    if (alarm_count > 0) {
        alarms[0].is_active = !alarms[0].is_active;
        lv_label_set_text(ui_Label12, alarms[0].is_active ? "已启用" : "未启用");
    }
}
static void screen4_toggle_alarm_1_cb(lv_event_t * e) {
    if (alarm_count > 1) {
        alarms[1].is_active = !alarms[1].is_active;
        lv_label_set_text(ui_Label11, alarms[1].is_active ? "已启用" : "未启用");
    }
}
static void screen4_toggle_alarm_2_cb(lv_event_t * e) {
    if (alarm_count > 2) {
        alarms[2].is_active = !alarms[2].is_active;
        lv_label_set_text(ui_Label10, alarms[2].is_active ? "已启用" : "未启用");
    }
}

// ================= 全局返回 =================
static void global_swipe_left_cb(lv_event_t * e) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT) {
        lv_disp_load_scr(ui_Screen1);
        if (animation_get_active_sequence() != 0) {
            animation_start();
        }
        g_is_on_screen1 = true;
    }
}

// ================= Screen5 事件：关闭与停止音乐 =================
static void screen5_close_btn_cb(lv_event_t * e) {
    audio_play_c_stop();
    current_ringing_alarm_idx = -1;
    vTaskDelay(pdMS_TO_TICKS(100)); // 【修复3】
    
    lv_disp_load_scr(ui_Screen1);
    if (animation_get_active_sequence() != 0) {
        animation_start();
    }
    g_is_on_screen1 = true;
}

static void screen5_delete_btn_cb(lv_event_t * e) {
    audio_play_c_stop();
    
    if (current_ringing_alarm_idx >= 0 && current_ringing_alarm_idx < 3) {
        int idx = current_ringing_alarm_idx;
        alarms[idx].is_active = false;
        alarms[idx].hour = 0;
        alarms[idx].minute = 0;
        alarms[idx].second = 0;
        
        switch(idx) {
            case 0: lv_label_set_text(ui_Label7, "--:--:--"); lv_label_set_text(ui_Label12, "未启用"); break;
            case 1: lv_label_set_text(ui_Label8, "--:--:--"); lv_label_set_text(ui_Label11, "未启用"); break;
            case 2: lv_label_set_text(ui_Label9, "--:--:--"); lv_label_set_text(ui_Label10, "未启用"); break;
        }
    }
    
    current_ringing_alarm_idx = -1;
    vTaskDelay(pdMS_TO_TICKS(100)); // 【修复3】
    
    lv_disp_load_scr(ui_Screen1);
    if (animation_get_active_sequence() != 0) {
        animation_start();
    }
    g_is_on_screen1 = true;
}

// ===== 屏幕状态监测与视频看护定时器 =====
static void screen_monitor_cb(lv_timer_t * timer) {
    lv_obj_t * current_scr = lv_disp_get_scr_act(NULL);
    bool is_screen1 = (current_scr == ui_Screen1);
    
    if (is_screen1 != g_is_on_screen1) {
        g_is_on_screen1 = is_screen1;
        if (is_screen1) {
            if (animation_get_active_sequence() != 0) {
                animation_start(); 
            }
        } else {
            animation_stop();  
        }
    }

    static bool last_state_was_screen1 = true;
    if (last_state_was_screen1 && !is_screen1) {
        lv_obj_set_style_bg_opa(current_scr, LV_OPA_COVER, 0);
        lv_obj_invalidate(current_scr);
    }
    last_state_was_screen1 = is_screen1;
}

// ================= 初始化 =================
void ui_custom_logic_init(void) {
    lv_obj_t * screens[] = {ui_Screen2, ui_Screen3, ui_Screen4, ui_Screen5};
    for(int i = 0; i < 4; i++) {
        if(screens[i] != NULL) {
            lv_obj_set_style_bg_opa(screens[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(screens[i], lv_color_hex(0x000000), 0); 
        }
    }

    audio_play_c_load_dir("/sdcard/music");
    scan_music_files();

    lv_timer_create(clock_timer_cb, 1000, NULL);
    lv_timer_create(screen_monitor_cb, 50, NULL);

    set_chinese_font();

    lv_label_set_text(ui_Label6, "应用");
    lv_label_set_text(ui_Label13, "下一张");
    lv_label_set_text(ui_Label16, "上一首");
    
    if(lv_obj_is_valid(ui_Label17)) {
        char song_name[128];
        audio_play_c_get_song_name(song_name, sizeof(song_name));
        lv_label_set_text(ui_Label17, song_name);
    }
    
    lv_label_set_text(ui_Label18, "下一首");
    lv_label_set_text(ui_Label7, "--:--:--");
    lv_label_set_text(ui_Label8, "--:--:--");
    lv_label_set_text(ui_Label9, "--:--:--");
    lv_label_set_text(ui_Label10, "待响铃");
    lv_label_set_text(ui_Label11, "待响铃");
    lv_label_set_text(ui_Label12, "待响铃");
    lv_label_set_text(ui_Label14, "应用");
    lv_label_set_text(ui_Label15, "自定义");
    
    if (lv_obj_is_valid(ui_Label1)) {
        lv_label_set_text(ui_Label1, "应用");
    }

    button_set_transparent(ui_Button1);
    button_set_transparent(ui_Button2);
    button_set_transparent(ui_Button3);
    button_set_transparent(ui_Button4);
    button_set_transparent(ui_Button5);
    button_set_transparent(ui_Button6);
    button_set_transparent(ui_Button7);
    button_set_transparent(ui_Button8);
    button_set_transparent(ui_Button9);
    
    if (lv_obj_is_valid(ui_Button12)) {
        button_set_transparent(ui_Button12);
    }

    lv_obj_add_event_cb(ui_Screen1, screen1_bg_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Button1, screen2_apply_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button2, screen2_next_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_add_event_cb(ui_Button3, screen3_prev_track_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button4, screen3_next_track_cb, LV_EVENT_CLICKED, NULL);
    
    if (lv_obj_is_valid(ui_Button12)) {
        lv_obj_add_event_cb(ui_Button12, screen3_apply_btn_cb, LV_EVENT_CLICKED, NULL);
    }
    
    lv_obj_add_event_cb(ui_Button9, screen4_apply_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button5, screen4_custom_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button6, screen4_toggle_alarm_0_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button8, screen4_toggle_alarm_1_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button7, screen4_toggle_alarm_2_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(ui_Screen2, global_swipe_left_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_Screen3, global_swipe_left_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_Screen4, global_swipe_left_cb, LV_EVENT_GESTURE, NULL);

    if(lv_obj_is_valid(ui_Roller1)) roller_set_transparent(ui_Roller1);
    if(lv_obj_is_valid(ui_Roller2)) roller_set_transparent(ui_Roller2);
    if(lv_obj_is_valid(ui_Roller3)) roller_set_transparent(ui_Roller3);

    button_set_transparent(ui_Button10);
    button_set_transparent(ui_Button11);
    
    lv_obj_add_event_cb(ui_Button10, screen5_close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button11, screen5_delete_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_set_style_text_font(ui_Label5, &my_font_chinese_24, 0);
    lv_obj_set_style_text_font(ui_Label19, &my_font_chinese_24, 0);
    lv_label_set_text(ui_Label5, "关闭闹钟");
    lv_label_set_text(ui_Label19, "关闭并删除");

    g_is_on_screen1 = true;
    if (animation_get_active_sequence() != 0) {
        animation_start();
    }
}