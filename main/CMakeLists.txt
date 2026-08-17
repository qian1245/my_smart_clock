# 1. 精准抓取 UI、Display、Board 以及新增的 Audio 文件夹的所有源码
file(GLOB UI_SRCS "ui/*.c" "ui/screens/*.c" "ui/components/*.c" "ui/fonts/*.c")
file(GLOB DISP_SRCS "display/*.c" "display/*.cc")
file(GLOB BOARD_SRCS "boards/woody/*.c" "boards/woody/*.cc" "boards/common/*.c" "boards/common/*.cc")

# 新增：抓取 audio 及其子目录下的源码（取消 processors 的全量抓取）
file(GLOB AUDIO_SRCS "audio/*.c" "audio/*.cc" 
                     "audio/codecs/*.c" "audio/codecs/*.cc" 
                     "audio/demuxer/*.c" "audio/demuxer/*.cc")

# 手动追加我们唯一需要的处理器源码，彻底将 afe_audio_processor 拒之门外
list(APPEND AUDIO_SRCS "audio/processors/no_audio_processor.cc")

idf_component_register(
    SRCS "main.cpp" "audio/mp3_decoder.cc" "audio/audio_player.cc" ${UI_SRCS} ${DISP_SRCS} ${BOARD_SRCS} ${AUDIO_SRCS}
    INCLUDE_DIRS "." "boards" "boards/woody" "boards/common" "display" "ui" "led" "ui/components" "ui/fonts" "ui/screens"
                 "audio" "audio/codecs" "audio/demuxer" "audio/processors"
)