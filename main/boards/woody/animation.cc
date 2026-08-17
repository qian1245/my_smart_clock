#include "animation.h"
#include "sd_card.h"
#include "esp_log.h"
#include "board.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_jpeg_dec.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include "ble.h"

static const char *TAG = "Animation";

// ================= 多动画目录管理 =================
static int g_current_upload_seq = 0;  // 当前正在上传的序列号
static int g_active_play_seq = 1;     // 【修改】默认开机锁定在 seq_1
static int g_total_seq_count = 1;     // 本地已有的序列总数，用于决定下一个上传创建的序号

// 动态获取序列文件夹路径
static void get_seq_dir_path(int seq_idx, char* out_path, size_t max_len) {
    snprintf(out_path, max_len, "%s/seq_%d", ANIMATION_DIR, seq_idx);
}

// 开机自动扫描 SD 卡中所有的动画文件夹，设定下一个编号，并固定播放 seq_1
static void scan_animation_sequences(void) {
    DIR *dir = opendir(ANIMATION_DIR);
    if (!dir) return;

    int max_seq = 0;
    int count = 0;
    bool has_seq_1 = false;
    struct dirent *entry;
    
    // 遍历 /sdcard/animation 目录，找出最大的 seq 序号
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            int seq_idx = -1;
            if (sscanf(entry->d_name, "seq_%d", &seq_idx) == 1) {
                count++;
                if (seq_idx > max_seq) {
                    max_seq = seq_idx;
                }
                if (seq_idx == 1) {
                    has_seq_1 = true;
                }
            }
        }
    }
    closedir(dir);

    if (count > 0) {
        // 【核心修改】：下一次上传导入的文件夹序号 = 当前已存在的最大序号 + 1 (例如已有 seq_1，下次就建 seq_2)
        g_total_seq_count = max_seq + 1; 
        
        // 【核心修改】：无论最新导入的是几，开机强行指定播放 seq_1 (如果有的话)
        g_active_play_seq = has_seq_1 ? 1 : max_seq; 
    } else {
        g_total_seq_count = 2; // 如果空卡，下一次上传就是 seq_2 (因为 seq_1 会被当做保底创建)
        g_active_play_seq = 1;
    }
    ESP_LOGI(TAG, "Auto-scanned: next_upload_seq=%d, auto_play_seq=%d", g_total_seq_count, g_active_play_seq);
}
// =======================================================

#define FRAME_BUF_SIZE (360 * 360 * 2)
#define NUM_BUFFERS    3
#define MAX_CACHED_FRAMES 150

typedef struct {
    uint16_t *buffer;
    uint16_t width;
    uint16_t height;
    int index;
    int sequence_id;
} animation_frame_t;

typedef struct {
    uint16_t *decoded_buffer;
    uint8_t *jpeg_data;
    size_t jpeg_size;
} cached_frame_t;

static animation_frame_t g_frames[NUM_BUFFERS];
static QueueHandle_t g_free_queue = NULL;
static QueueHandle_t g_ready_queue = NULL;

static TaskHandle_t g_playback_task_handle = NULL;
static TaskHandle_t g_decode_task_handle = NULL;

int g_max_index = -1;
int g_sequence_id = 0;
static volatile bool g_running = false;

bool g_seq_changed = false;
volatile bool g_needs_reindex = true; 

static cached_frame_t g_cached_frames[MAX_CACHED_FRAMES];
static int g_cached_frame_count = 0;

static draw_callback_t g_draw_callback = NULL;

static void free_cached_frames(void) {
    for (int i = 0; i < MAX_CACHED_FRAMES; i++) {
        if (g_cached_frames[i].decoded_buffer != NULL) {
            heap_caps_free(g_cached_frames[i].decoded_buffer);
            g_cached_frames[i].decoded_buffer = NULL;
        }
        if (g_cached_frames[i].jpeg_data != NULL) {
            heap_caps_free(g_cached_frames[i].jpeg_data);
            g_cached_frames[i].jpeg_data = NULL;
        }
        g_cached_frames[i].jpeg_size = 0;
    }
    g_cached_frame_count = 0;
}

static esp_err_t decode_jpeg_memory(const uint8_t *jpeg_data, size_t jpeg_size, uint16_t *out_rgb_buf, uint16_t *out_width, uint16_t *out_height) {
    jpeg_dec_config_t config = {}; 
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    config.rotate = JPEG_ROTATE_0D;
    config.block_enable = false;

    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_error_t ret = jpeg_dec_open(&config, &jpeg_dec);
    if (ret != JPEG_ERR_OK) {
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = {}; 
    io.inbuf = (uint8_t *)jpeg_data;
    io.inbuf_len = (int)jpeg_size;
    io.outbuf = (uint8_t *)out_rgb_buf;
    io.out_size = FRAME_BUF_SIZE;

    jpeg_dec_header_info_t header_info;
    ret = jpeg_dec_parse_header(jpeg_dec, &io, &header_info);
    if (ret != JPEG_ERR_OK) {
        jpeg_dec_close(jpeg_dec);
        return ESP_FAIL;
    }

    *out_width = header_info.width;
    *out_height = header_info.height;

    ret = jpeg_dec_process(jpeg_dec, &io);
    jpeg_dec_close(jpeg_dec);

    if (ret != JPEG_ERR_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void purge_queues(void) {
    if (g_ready_queue == NULL || g_free_queue == NULL) {
        return;
    }

    animation_frame_t *frame;
    while (xQueueReceive(g_ready_queue, &frame, 0) == pdTRUE) {
        if (g_free_queue != NULL) {
            xQueueSend(g_free_queue, &frame, 0);
        }
    }
}

static void decode_task(void *pvParameters) {
    int decode_file_idx = 0;
    while (1) {
        if (g_free_queue == NULL || g_ready_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        animation_frame_t *frame = NULL;
        if (xQueueReceive(g_free_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        while (!g_running || !sd_card_is_inserted() || g_max_index < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            decode_file_idx = 0;
        }

        if (frame == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (decode_file_idx < g_cached_frame_count) {
            int current_seq = g_sequence_id;
            esp_err_t ret = ESP_FAIL;

            if (g_running && g_cached_frames[decode_file_idx].jpeg_data != NULL) {
                ret = decode_jpeg_memory(g_cached_frames[decode_file_idx].jpeg_data, 
                                         g_cached_frames[decode_file_idx].jpeg_size, 
                                         frame->buffer, &frame->width, &frame->height);
            }

            if (ret == ESP_OK && g_running && current_seq == g_sequence_id) {
                frame->index = decode_file_idx;
                frame->sequence_id = current_seq;
                xQueueSend(g_ready_queue, &frame, portMAX_DELAY);

                decode_file_idx++;
                if (decode_file_idx > g_max_index) {
                    decode_file_idx = 0;
                }
                vTaskDelay(1);
            } else {
                if (g_free_queue != NULL) {
                    xQueueSend(g_free_queue, &frame, portMAX_DELAY);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            if (g_free_queue != NULL) {
                xQueueSend(g_free_queue, &frame, portMAX_DELAY);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            decode_file_idx = 0;
        }
    }
}

static void playback_task(void *pvParameters) {
    const int64_t frame_interval_us = 33333; 
    bool last_inserted = sd_card_is_inserted();
    
    int64_t last_fps_time = esp_timer_get_time();
    int64_t next_frame_time = esp_timer_get_time();

    while (1) {
        if (g_seq_changed) {
            g_max_index = -1; 
            vTaskDelay(pdMS_TO_TICKS(150)); 
            g_needs_reindex = true;     
            g_cached_frame_count = 0; 
            g_seq_changed = false;    
        }

        bool currently_inserted = sd_card_is_inserted();
        if (!currently_inserted) {
            if (animation_has_files()) {
                currently_inserted = true;
            }
        }

        if (currently_inserted != last_inserted) {
            if (currently_inserted) {
                sd_card_mount();
                g_needs_reindex = true;
            } else {
                g_max_index = -1;
                g_sequence_id++;
                purge_queues();
                free_cached_frames();
                g_needs_reindex = true;
            }
            last_inserted = currently_inserted;
        }

        if (!currently_inserted) {
            animation_stop(); 
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!g_running) {
            vTaskDelay(pdMS_TO_TICKS(100));
            next_frame_time = esp_timer_get_time();
            continue;
        }

        if (g_needs_reindex) {
            if (g_cached_frame_count > 0) {
                g_max_index = g_cached_frame_count - 1;
                g_needs_reindex = false;
                next_frame_time = esp_timer_get_time();
                continue;
            }

            g_max_index = -1;
            g_sequence_id++;
            purge_queues();
            free_cached_frames(); 

            char active_dir[128];
            get_seq_dir_path(g_active_play_seq, active_dir, sizeof(active_dir));

            char zero_path[256];
            snprintf(zero_path, sizeof(zero_path), "%s/0.jpg", active_dir);

            bool zero_exists = false;
            for (int r = 0; r < 10; r++) { 
                if (!g_running) break;
                if (sd_card_file_exists(zero_path)) {
                    zero_exists = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            if (!zero_exists || !g_running) {
                if (!g_running) continue;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            int idx = 0;
            while (idx < MAX_CACHED_FRAMES && g_running) {
                char path[256];
                snprintf(path, sizeof(path), "%s/%d.jpg", active_dir, idx);

                FILE *f = fopen(path, "rb");
                if (!f) break;

                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);

                if (fsize <= 0) {
                    fclose(f);
                    break;
                }

                uint8_t *buf = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
                if (!buf) {
                    fclose(f);
                    break;
                }

                size_t read_bytes = fread(buf, 1, fsize, f);
                fclose(f);

                if (read_bytes != fsize) {
                    heap_caps_free(buf);
                    break;
                }

                g_cached_frames[idx].decoded_buffer = NULL;
                g_cached_frames[idx].jpeg_data = buf;
                g_cached_frames[idx].jpeg_size = fsize;
                idx++;
            }

            if (!g_running) {
                free_cached_frames();
                continue;
            }

            g_cached_frame_count = idx;
            g_max_index = g_cached_frame_count - 1;
            g_needs_reindex = false;
            next_frame_time = esp_timer_get_time();
        }

        if (g_max_index >= 0 && g_running) {
            if (g_ready_queue == NULL) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            animation_frame_t *frame = NULL;
            if (xQueueReceive(g_ready_queue, &frame, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (frame != NULL && frame->sequence_id == g_sequence_id && frame->index <= g_max_index && g_running) {
                    int64_t now = esp_timer_get_time();
                    if (now < next_frame_time) {
                        int64_t wait_time = next_frame_time - now;
                        if (wait_time > 1000) { 
                            vTaskDelay(pdMS_TO_TICKS(wait_time / 1000));
                        } else {
                            while (esp_timer_get_time() < next_frame_time) {
                                esp_rom_delay_us(10);
                            }
                        }
                    }

                    if (g_draw_callback != NULL) {
                        g_draw_callback(frame->buffer, frame->width, frame->height);
                    }
                    
                    now = esp_timer_get_time();
                    if (now - last_fps_time >= 1000000) {
                        last_fps_time = now;
                    }

                    next_frame_time += frame_interval_us;
                    now = esp_timer_get_time();
                    if (now > next_frame_time + frame_interval_us * 2) {
                        next_frame_time = now + frame_interval_us;
                    }
                }
                
                if (g_free_queue != NULL && frame != NULL) {
                    xQueueSend(g_free_queue, &frame, portMAX_DELAY);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

extern "C" void animation_handle_ble_rx(uint8_t *data, uint16_t len);

esp_err_t animation_init(draw_callback_t draw_cb) {
    g_draw_callback = draw_cb;

    for (int i = 0; i < MAX_CACHED_FRAMES; i++) {
        g_cached_frames[i].decoded_buffer = NULL;
        g_cached_frames[i].jpeg_data = NULL;
        g_cached_frames[i].jpeg_size = 0;
    }
    g_cached_frame_count = 0;

    if (g_free_queue == NULL) {
        g_free_queue = xQueueCreate(NUM_BUFFERS, sizeof(animation_frame_t *));
    }
    if (g_ready_queue == NULL) {
        g_ready_queue = xQueueCreate(NUM_BUFFERS, sizeof(animation_frame_t *));
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_frames[i].buffer == NULL) {
            g_frames[i].buffer = (uint16_t *)heap_caps_aligned_alloc(16, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
            if (!g_frames[i].buffer) {
                return ESP_ERR_NO_MEM;
            }
            g_frames[i].width = 0;
            g_frames[i].height = 0;
            g_frames[i].index = -1;
            g_frames[i].sequence_id = -1;
            animation_frame_t *p_frame = &g_frames[i];
            
            if (g_free_queue != NULL) {
                xQueueSend(g_free_queue, &p_frame, portMAX_DELAY);
            }
        }
    }

    sd_card_init();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t mount_ret = sd_card_mount();
    if (mount_ret == ESP_OK) {
        struct stat st;
        if (stat(ANIMATION_DIR, &st) != 0) {
            mkdir(ANIMATION_DIR, 0777);
        }
        
        // 【修改】保底默认创建 seq_1
        char default_path[128];
        get_seq_dir_path(1, default_path, sizeof(default_path));
        if (stat(default_path, &st) != 0) {
            mkdir(default_path, 0777);
        }

        // 开机扫描计算下一个文件编号，并把播放目标强制锁定为 seq_1
        scan_animation_sequences();
    }

    if (g_playback_task_handle == NULL) {
        size_t stack_size = 4096;
        StackType_t *stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        StaticTask_t *buffer = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        g_playback_task_handle = xTaskCreateStaticPinnedToCore(playback_task, "anim_play", stack_size, NULL, 5, stack, buffer, 0);
    }
    if (g_decode_task_handle == NULL) {
        size_t stack_size = 4096;
        StackType_t *stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        StaticTask_t *buffer = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        g_decode_task_handle = xTaskCreateStaticPinnedToCore(decode_task, "anim_decode", stack_size, NULL, 5, stack, buffer, 1);
    }

    return ESP_OK;
}

extern "C" void animation_start(void) {
    if (g_running) return;
    g_sequence_id++;
    if (g_cached_frame_count > 0) {
        g_max_index = g_cached_frame_count - 1;
    } else {
        g_max_index = -1;
    }
    g_running = true;
}

extern "C" void animation_stop(void) {
    if (!g_running) return;
    g_running = false;
    g_sequence_id++;
    g_max_index = -1;
    purge_queues();
}

void animation_deinit(void) {
    ble_stop();
    g_running = false;
    g_sequence_id++;
    g_max_index = -1;

    if (g_playback_task_handle != NULL) {
        vTaskDelete(g_playback_task_handle);
        g_playback_task_handle = NULL;
    }
    if (g_decode_task_handle != NULL) {
        vTaskDelete(g_decode_task_handle);
        g_decode_task_handle = NULL;
    }

    purge_queues();
    free_cached_frames();

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_frames[i].buffer != NULL) {
            heap_caps_free(g_frames[i].buffer);
            g_frames[i].buffer = NULL;
        }
    }

    if (g_free_queue != NULL) {
        vQueueDelete(g_free_queue);
        g_free_queue = NULL;
    }
    if (g_ready_queue != NULL) {
        vQueueDelete(g_ready_queue);
        g_ready_queue = NULL;
    }

    sd_card_unmount();
}

bool animation_is_running(void) {
    return g_running;
}

static FILE* g_current_file = NULL;
static int g_current_index = -1;
static bool g_transfer_active = false;

static void send_proto_response(uint8_t type, int index, uint32_t offset, uint8_t status) {
    uint8_t resp[10];
    resp[0] = type;
    memcpy(&resp[1], &index, 4);
    memcpy(&resp[5], &offset, 4);
    resp[9] = status;
    ble_send_data(resp, 10);
}

static esp_err_t file_manager_write_at_offset(FILE *f, size_t offset, const uint8_t *data, size_t len) {
    if (!f) return ESP_ERR_INVALID_ARG;
    
    long curr = ftell(f);
    if (curr < 0 || (size_t)curr != offset) {
        if (fseek(f, offset, SEEK_SET) != 0) {
            return ESP_FAIL;
        }
    }
    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

#define ANIM_PROTO_START       0x01
#define ANIM_PROTO_FILE_INFO   0x02
#define ANIM_PROTO_DATA_CHUNK  0x03
#define ANIM_PROTO_FILE_DONE   0x04
#define ANIM_PROTO_ALL_DONE    0x06
#define ANIM_PROTO_CLEAR       0x07

extern "C" void animation_set_active_sequence(int seq_idx);

extern "C" bool g_ble_transfer_active;
bool g_ble_transfer_active = false;

extern "C" void restore_xiaozhi_after_ble_transfer(void) {
    if (g_ble_transfer_active) {
        g_ble_transfer_active = false;
    }
}

extern "C" void animation_handle_ble_rx(uint8_t *data, uint16_t len) {
    if (len < 1) return;
    uint8_t type = data[0];

    switch (type) {
        case ANIM_PROTO_START: {
            g_transfer_active = true;
            g_ble_transfer_active = true;
            animation_stop();
            ble_update_conn_params(true);

            // 此处直接引用启动时算好的递增编号（此时一定是已存在的最大文件夹编号+1）
            g_current_upload_seq = g_total_seq_count;
            g_total_seq_count++;
            
            struct stat st;
            if (stat(ANIMATION_DIR, &st) != 0) mkdir(ANIMATION_DIR, 0777);

            char new_dir[128];
            get_seq_dir_path(g_current_upload_seq, new_dir, sizeof(new_dir));
            mkdir(new_dir, 0777); 

            send_proto_response(ANIM_PROTO_START, 0, 0, 1);
            break;
        }

        case ANIM_PROTO_FILE_INFO: {
            if (len < 9) return;
            int index;
            uint32_t size;
            memcpy(&index, &data[1], 4);
            memcpy(&size, &data[5], 4);
            
            char path[128];
            get_seq_dir_path(g_current_upload_seq, path, sizeof(path));
            char file_path[256];
            snprintf(file_path, sizeof(file_path), "%s/%d.jpg", path, index);
            
            if (g_current_file) fclose(g_current_file);
            g_current_file = fopen(file_path, "wb");
            if (g_current_file && setvbuf(g_current_file, NULL, _IOFBF, 4096) != 0) {
            }
            g_current_index = index;
            
            send_proto_response(ANIM_PROTO_FILE_INFO, index, 0, g_current_file ? 1 : 0);
            break;
        }

        case ANIM_PROTO_DATA_CHUNK: {
            if (len < 17) return; 
            int index;
            uint32_t offset, total_size, chunk_size;
            memcpy(&index, &data[1], 4);
            memcpy(&offset, &data[5], 4);
            memcpy(&total_size, &data[9], 4);
            memcpy(&chunk_size, &data[13], 4);

            if (g_current_index == index && g_current_file) {
                esp_err_t ret = file_manager_write_at_offset(g_current_file, offset, &data[17], chunk_size);
                send_proto_response(ANIM_PROTO_DATA_CHUNK, index, offset, (ret == ESP_OK) ? 1 : 0);
            } else {
                send_proto_response(ANIM_PROTO_DATA_CHUNK, index, offset, 0);
            }
            break;
        }

        case ANIM_PROTO_FILE_DONE: {
            if (len < 5) return;
            int index;
            memcpy(&index, &data[1], 4);
            if (g_current_index == index && g_current_file) {
                fflush(g_current_file);
                fsync(fileno(g_current_file));
                fclose(g_current_file);
                g_current_file = NULL;
                g_current_index = -1;
            }
            send_proto_response(ANIM_PROTO_FILE_DONE, index, 0, 1);
            break;
        }

        case ANIM_PROTO_ALL_DONE: {
            g_transfer_active = false;
            if (g_current_file) {
                fclose(g_current_file);
                g_current_file = NULL;
            }
            ble_update_conn_params(false);
            send_proto_response(ANIM_PROTO_ALL_DONE, 0, 0, 1);
            
            // 上传完毕后，自动开始播放刚刚导入的新视频
            animation_set_active_sequence(g_current_upload_seq);           
            animation_start();
            restore_xiaozhi_after_ble_transfer();
            break;
        }

        case ANIM_PROTO_CLEAR: {
            animation_stop();
            send_proto_response(ANIM_PROTO_CLEAR, 0, 0, 1);
            break;
        }

        default:
            break;
    }
}

static bool g_ble_allowed = false;
static bool g_ble_initialized = false;

void animation_enable_ble_init(void) {
    g_ble_allowed = true;
}

void animation_start_ble(void) {
    if (!g_ble_allowed) return;
    if (!g_ble_initialized) {
        esp_err_t ble_ret = ble_init();
        if (ble_ret == ESP_OK) {
            ble_set_rx_callback(animation_handle_ble_rx);
            g_ble_initialized = true;
        }
    } else {
        ble_start();
    }
}

void animation_stop_ble(void) {
    if (g_ble_allowed && g_ble_initialized) {
        ble_deinit();
        g_ble_initialized = false;
    }
}

bool animation_has_files(void) {
    char zero_path[128];
    get_seq_dir_path(g_active_play_seq, zero_path, sizeof(zero_path));
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/0.jpg", zero_path);
    return sd_card_file_exists(file_path);
}

// ================= 暴露给 UI 的接口 =================

extern "C" int animation_get_total_sequences(void) {
    return g_total_seq_count;
}

extern "C" void animation_set_active_sequence(int seq_idx) {
    if (g_active_play_seq != seq_idx) {
        g_active_play_seq = seq_idx;
        g_seq_changed = true; 
        animation_start();
    }
}

extern "C" uint16_t* animation_get_preview_frame(int seq_idx) {
    char path[128];
    get_seq_dir_path(seq_idx, path, sizeof(path));
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/0.jpg", path);
    
    FILE *f = fopen(file_path, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *jpg_buf = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!jpg_buf) {
        fclose(f);
        return NULL;
    }
    fread(jpg_buf, 1, fsize, f);
    fclose(f);

   uint16_t *rgb_buf = (uint16_t *)heap_caps_aligned_alloc(16, 360 * 360 * 2, MALLOC_CAP_SPIRAM);
    if (rgb_buf) {
        uint16_t w, h;
        if (decode_jpeg_memory(jpg_buf, fsize, rgb_buf, &w, &h) != ESP_OK) {
            heap_caps_free(rgb_buf);
            rgb_buf = NULL; 
        } else {
            int total_pixels = w * h;
            for (int i = 0; i < total_pixels; i++) {
                rgb_buf[i] = (rgb_buf[i] >> 8) | (rgb_buf[i] << 8);
            }
        }
    }
    heap_caps_free(jpg_buf);
    
    return rgb_buf; 
}

extern "C" int animation_get_active_sequence(void) {
    return g_active_play_seq;
}