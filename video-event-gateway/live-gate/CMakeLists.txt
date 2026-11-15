cmake_minimum_required(VERSION 3.16)
project(veg_live_gate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(FFMPEG_DEV_ROOT "" CACHE PATH "FFmpeg SDK prefix, e.g. /opt/orangepi/kodi-ffmpeg")

if(FFMPEG_DEV_ROOT)
  find_path(AVFORMAT_INCLUDE_DIR NAMES libavformat/avformat.h
    PATHS "${FFMPEG_DEV_ROOT}/include"
          "${FFMPEG_DEV_ROOT}/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}"
    NO_DEFAULT_PATH)
  foreach(component avformat avcodec avutil swresample)
    find_library(${component}_LIBRARY NAMES ${component}
      PATHS "${FFMPEG_DEV_ROOT}/lib"
            "${FFMPEG_DEV_ROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
      NO_DEFAULT_PATH)
  endforeach()
else()
  find_path(AVFORMAT_INCLUDE_DIR NAMES libavformat/avformat.h)
  foreach(component avformat avcodec avutil swresample)
    find_library(${component}_LIBRARY NAMES ${component})
  endforeach()
endif()

foreach(required AVFORMAT_INCLUDE_DIR avformat_LIBRARY avcodec_LIBRARY avutil_LIBRARY swresample_LIBRARY)
  if(NOT ${required})
    message(FATAL_ERROR "Missing ${required}; set FFMPEG_DEV_ROOT to the board FFmpeg SDK")
  endif()
endforeach()

add_executable(veg_live_gate ../src/live_gate.cpp audio_asr.cpp)
target_include_directories(veg_live_gate PRIVATE ${AVFORMAT_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(veg_live_gate PRIVATE ${avformat_LIBRARY} ${avcodec_LIBRARY} ${avutil_LIBRARY} ${swresample_LIBRARY})
target_compile_options(veg_live_gate PRIVATE -Wall -Wextra -Wpedantic)
if(FFMPEG_DEV_ROOT)
  set_target_properties(veg_live_gate PROPERTIES BUILD_RPATH "${FFMPEG_DEV_ROOT}/lib")
endif()
