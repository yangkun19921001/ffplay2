cmake_minimum_required(VERSION 3.19)
project(ffplay2)
set(CMAKE_CXX_STANDARD 11)

set(PATH ${CMAKE_CURRENT_SOURCE_DIR})

#add sub dir
add_subdirectory(src)

#ffmpeg
set(FFMPEG_HOME /usr/local/Cellar/ffmpeg@4/4.4.1)
set(FFMPEG_INCLUDE_DIRS ${FFMPEG_HOME}/include/)
set(FFMPEG_LIBRARIES ${FFMPEG_HOME}/lib/)
include_directories(${FFMPEG_INCLUDE_DIRS})
link_directories(${FFMPEG_LIBRARIES})

#sdl2
set(SDL2_HOME ${PATH}/thirdparties/sdl2.0.14)
add_subdirectory(${SDL2_HOME})
include_directories(${SDL2_HOME}/include/)


#unit packet-queue test
set(QUEUE_PACKET_TEST test/ff_packet_queue_test.c)
add_executable(${PROJECT_NAME}-packet_queue_debug ${QUEUE_PACKET_TEST} ${src})
target_link_libraries(${PROJECT_NAME}-packet_queue_debug
        avcodec avdevice avfilter avformat avutil swscale swresample SDL2
        )
#unit frame-queue test
set(QUEUE_FRAME_TEST test/ff_frame_queue_test.c)
add_executable(${PROJECT_NAME}-frame_queue_debug ${QUEUE_FRAME_TEST} ${src})
target_link_libraries(${PROJECT_NAME}-frame_queue_debug
        avcodec avdevice avfilter avformat avutil swscale swresample SDL2
        )
#unit DEMUXER
set(QDEMUXER_TEST test/ff_demuxer_test.c)
add_executable(${PROJECT_NAME}-demuxer_debug ${QDEMUXER_TEST} ${src})
target_link_libraries(${PROJECT_NAME}-demuxer_debug
        avcodec avdevice avfilter avformat avutil swscale swresample SDL2
        )
#unit DECODER
set(DECODER_TEST test/ff_decoder_test.c)
add_executable(${PROJECT_NAME}-decoder_debug ${DECODER_TEST} ${src})
target_link_libraries(${PROJECT_NAME}-decoder_debug
        avcodec avdevice avfilter avformat avutil swscale swresample SDL2
        )

#ffplay2 debug
add_executable(${PROJECT_NAME}-debug main.c ${src})
target_link_libraries(${PROJECT_NAME}-debug
        avcodec avdevice avfilter avformat avutil swscale swresample SDL2
        )