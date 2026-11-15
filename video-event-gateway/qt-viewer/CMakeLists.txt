cmake_minimum_required(VERSION 3.16)
project(veg_qt_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
  gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
add_executable(veg_qt_viewer main.cpp)
target_link_libraries(veg_qt_viewer PRIVATE Qt6::Widgets PkgConfig::GSTREAMER)
target_compile_options(veg_qt_viewer PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS veg_qt_viewer RUNTIME DESTINATION bin)
