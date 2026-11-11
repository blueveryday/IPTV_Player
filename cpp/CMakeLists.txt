cmake_minimum_required(VERSION 3.21)
project(IPTVPlayer VERSION 2026.10.12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Core5Compat)
find_package(ZLIB REQUIRED)

# ---- libmpv -------------------------------------------------------------
# Linux/macOS: pkg-config.  Windows: set -DMPV_ROOT=<解压后的 libmpv 开发包目录>
set(MPV_ROOT "" CACHE PATH "libmpv development package root (contains include/mpv/client.h)")
find_package(PkgConfig QUIET)
set(MPV_FOUND FALSE)
if(PkgConfig_FOUND AND NOT MPV_ROOT)
  pkg_check_modules(MPV QUIET IMPORTED_TARGET mpv)
endif()
if(MPV_FOUND)
  set(MPV_TARGET PkgConfig::MPV)
else()
  find_path(MPV_INCLUDE_DIR mpv/client.h HINTS "${MPV_ROOT}/include" "${MPV_ROOT}")
  find_library(MPV_LIBRARY NAMES mpv libmpv mpv-2 libmpv-2
               HINTS "${MPV_ROOT}" "${MPV_ROOT}/lib" "${MPV_ROOT}/lib/x64")
  if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
    message(FATAL_ERROR "libmpv not found. Install libmpv-dev or pass -DMPV_ROOT=<path>")
  endif()
  add_library(mpv_imported UNKNOWN IMPORTED)
  set_target_properties(mpv_imported PROPERTIES
    IMPORTED_LOCATION "${MPV_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}")
  set(MPV_TARGET mpv_imported)
endif()

set(SOURCES
  src/main.cpp
  src/resources.qrc
  src/util.cpp
  src/http.cpp
  src/hlsproxy.cpp
  src/webdav.cpp
  src/epg.cpp
  src/config.cpp
  src/widgets.cpp
  src/mpvwidget.cpp
  src/mainwindow.cpp
  src/mainwindow_ui.cpp
  src/mainwindow_play.cpp
  src/mainwindow_record.cpp
  src/mainwindow_epg.cpp
  src/mainwindow_webdav.cpp
  src/mainwindow_dialogs.cpp
)

add_executable(IPTVPlayer WIN32 ${SOURCES})
target_link_libraries(IPTVPlayer PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Core5Compat
  ZLIB::ZLIB ${MPV_TARGET})
target_compile_definitions(IPTVPlayer PRIVATE QT_NO_CAST_FROM_BYTEARRAY)
if(MSVC)
  target_compile_options(IPTVPlayer PRIVATE /utf-8 /W3)
else()
  target_compile_options(IPTVPlayer PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 运行时资源：与 Python 版相同的目录约定（可执行文件同目录）
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_custom_command(TARGET IPTVPlayer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:IPTVPlayer>/src"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${REPO_ROOT}/src/asset" "$<TARGET_FILE_DIR:IPTVPlayer>/src/asset"
  COMMENT "Copying runtime assets")
file(GLOB IPTV_M3U_FILES "${REPO_ROOT}/iptv_*.m3u")
foreach(f ${IPTV_M3U_FILES})
  add_custom_command(TARGET IPTVPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${f}" "$<TARGET_FILE_DIR:IPTVPlayer>/")
endforeach()

install(TARGETS IPTVPlayer RUNTIME DESTINATION .)
install(DIRECTORY "${REPO_ROOT}/src/asset" DESTINATION src)
install(FILES ${IPTV_M3U_FILES} DESTINATION .)
