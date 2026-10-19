cmake_minimum_required(VERSION 3.16)
project(video_event_gateway VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV 4 QUIET COMPONENTS core imgproc imgcodecs videoio)
if(NOT OpenCV_FOUND)
  # Ubuntu splits these libraries into small -dev packages; their CMake config
  # is shipped by the much larger libopencv-dev metapackage.
  find_path(OpenCV_INCLUDE_DIRS NAMES opencv2/core.hpp PATH_SUFFIXES opencv4)
  foreach(component core imgproc imgcodecs videoio)
    find_library(OPENCV_LIB_${component} NAMES opencv_${component})
    if(NOT OPENCV_LIB_${component})
      message(FATAL_ERROR "OpenCV ${component} library is missing")
    endif()
    list(APPEND OpenCV_LIBS "${OPENCV_LIB_${component}}")
  endforeach()
  if(NOT OpenCV_INCLUDE_DIRS)
    message(FATAL_ERROR "OpenCV headers are missing")
  endif()
endif()

add_library(veg_core src/core.cpp)
target_include_directories(veg_core PUBLIC include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(veg_core PUBLIC ${OpenCV_LIBS})
target_compile_options(veg_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(veg_detect src/main.cpp)
target_link_libraries(veg_detect PRIVATE veg_core)
target_compile_options(veg_detect PRIVATE -Wall -Wextra -Wpedantic)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GST_RTSP QUIET IMPORTED_TARGET gstreamer-rtsp-server-1.0)
endif()
if(GST_RTSP_FOUND)
  add_executable(veg_rtsp_server src/rtsp_server.cpp)
  target_link_libraries(veg_rtsp_server PRIVATE PkgConfig::GST_RTSP)
  target_compile_options(veg_rtsp_server PRIVATE -Wall -Wextra -Wpedantic)
  install(TARGETS veg_rtsp_server RUNTIME DESTINATION bin)
else()
  message(STATUS "GStreamer RTSP server development files absent; RTSP server target skipped")
endif()

if(NOT CMAKE_CROSSCOMPILING)
  # Ubuntu's split Drogon -dev package omits CMake dependencies for optional
  # database backends. Link its shared library directly for this HTTP-only app.
  find_path(DROGON_INCLUDE_DIR NAMES drogon/drogon.h)
  find_path(JSONCPP_INCLUDE_DIR NAMES json/json.h PATH_SUFFIXES jsoncpp)
  find_library(DROGON_LIBRARY NAMES drogon)
  find_library(TRANTOR_LIBRARY NAMES trantor)
  find_library(JSONCPP_LIBRARY NAMES jsoncpp)
endif()
if(DROGON_INCLUDE_DIR AND JSONCPP_INCLUDE_DIR AND DROGON_LIBRARY AND
   TRANTOR_LIBRARY AND JSONCPP_LIBRARY)
  add_executable(veg_gateway src/gateway.cpp)
  target_include_directories(veg_gateway PRIVATE ${DROGON_INCLUDE_DIR} ${JSONCPP_INCLUDE_DIR})
  target_link_libraries(veg_gateway PRIVATE veg_core ${DROGON_LIBRARY}
                        ${TRANTOR_LIBRARY} ${JSONCPP_LIBRARY})
  set(NCNN_ROOT "" CACHE PATH "Prefix of an optional ncnn installation")
  if(NCNN_ROOT)
    find_path(NCNN_INCLUDE_DIR NAMES net.h HINTS ${NCNN_ROOT}/include
              PATH_SUFFIXES ncnn)
    find_library(NCNN_LIBRARY NAMES ncnn HINTS ${NCNN_ROOT}/lib)
    if(NCNN_INCLUDE_DIR AND NCNN_LIBRARY)
      target_sources(veg_gateway PRIVATE third_party/yolov8_ncnn/yoloV8.cpp)
      target_include_directories(veg_gateway PRIVATE ${NCNN_INCLUDE_DIR}
                                 third_party/yolov8_ncnn)
      target_link_libraries(veg_gateway PRIVATE ${NCNN_LIBRARY})
      target_compile_definitions(veg_gateway PRIVATE VEG_WITH_NCNN=1)
      find_package(OpenMP QUIET)
      if(OpenMP_CXX_FOUND)
        target_link_libraries(veg_gateway PRIVATE OpenMP::OpenMP_CXX)
      endif()
    else()
      message(FATAL_ERROR "NCNN_ROOT provided, but ncnn headers or library missing")
    endif()
  endif()
  target_compile_options(veg_gateway PRIVATE -Wall -Wextra -Wpedantic)
  install(TARGETS veg_gateway RUNTIME DESTINATION bin)
else()
  message(STATUS "Drogon development files absent; gateway target skipped")
endif()

if(BUILD_TESTING)
  add_executable(veg_generate_fixture tests/generate_fixture.cpp)
  target_link_libraries(veg_generate_fixture PRIVATE ${OpenCV_LIBS})
  target_include_directories(veg_generate_fixture PRIVATE ${OpenCV_INCLUDE_DIRS})

  add_executable(veg_generate_person_fixture tests/generate_person_fixture.cpp)
  target_link_libraries(veg_generate_person_fixture PRIVATE ${OpenCV_LIBS})
  target_include_directories(veg_generate_person_fixture PRIVATE ${OpenCV_INCLUDE_DIRS})

  add_executable(veg_rule_test tests/rule_test.cpp)
  target_link_libraries(veg_rule_test PRIVATE veg_core)
  add_test(NAME rules COMMAND veg_rule_test)

  add_test(
    NAME synthetic_video
    COMMAND ${CMAKE_COMMAND}
      -DGENERATE=$<TARGET_FILE:veg_generate_fixture>
      -DDETECT=$<TARGET_FILE:veg_detect>
      -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/synthetic_video
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_fixture.cmake)

  if(TARGET veg_rtsp_server)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(
        NAME rtsp_loopback
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rtsp.py
          --server $<TARGET_FILE:veg_rtsp_server>
          --detect $<TARGET_FILE:veg_detect>
          --generate $<TARGET_FILE:veg_generate_fixture>
          --test-dir ${CMAKE_CURRENT_BINARY_DIR}/rtsp_loopback)
      set_tests_properties(rtsp_loopback PROPERTIES TIMEOUT 45)
    endif()
  endif()

  if(TARGET veg_gateway AND TARGET veg_rtsp_server AND Python3_Interpreter_FOUND)
    add_test(
      NAME gateway_loopback
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_gateway.py
        --server $<TARGET_FILE:veg_rtsp_server>
        --gateway $<TARGET_FILE:veg_gateway>
        --generate $<TARGET_FILE:veg_generate_fixture>
        --test-dir ${CMAKE_CURRENT_BINARY_DIR}/gateway_loopback)
    set_tests_properties(gateway_loopback PROPERTIES TIMEOUT 45)
  endif()
  set(NCNN_SAMPLE_IMAGE "" CACHE FILEPATH "Optional person image for ncnn integration test")
  set(NCNN_MODEL_PARAM "" CACHE FILEPATH "Optional ncnn YOLOv8 param for test")
  set(NCNN_MODEL_BIN "" CACHE FILEPATH "Optional ncnn YOLOv8 weights for test")
  if(TARGET veg_gateway AND NCNN_LIBRARY AND Python3_Interpreter_FOUND AND
     EXISTS "${NCNN_SAMPLE_IMAGE}" AND EXISTS "${NCNN_MODEL_PARAM}" AND
     EXISTS "${NCNN_MODEL_BIN}")
    add_test(
      NAME ncnn_gateway
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ai_gateway.py
        --gateway $<TARGET_FILE:veg_gateway>
        --generate $<TARGET_FILE:veg_generate_person_fixture>
        --image ${NCNN_SAMPLE_IMAGE}
        --param ${NCNN_MODEL_PARAM}
        --bin ${NCNN_MODEL_BIN}
        --test-dir ${CMAKE_CURRENT_BINARY_DIR}/ncnn_gateway)
    set_tests_properties(ncnn_gateway PROPERTIES TIMEOUT 90)
  endif()
endif()

install(TARGETS veg_detect RUNTIME DESTINATION bin)
install(FILES config/example.yaml DESTINATION share/video-event-gateway)
