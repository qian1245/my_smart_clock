/* audio_player_c_api.h */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 这里提供给 .c 文件的纯 C 函数声明 */
void audio_play_c_load_dir(const char* dir_path);
void audio_play_c_play(void);
void audio_play_c_pause(void);
void audio_play_c_stop(void);
void audio_play_c_next(void);
void audio_play_c_prev(void);
bool audio_play_c_is_playing(void);

/* C 语言没有 std::string，用字符数组拷贝代替 */
void audio_play_c_get_song_name(char* buffer, int max_len);

#ifdef __cplusplus
}
#endif