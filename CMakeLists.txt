cmake_minimum_required(VERSION 3.25)

project(whiz LANGUAGES CXX VERSION 3.4.2)

include(FetchContent)

# ---------------------------------------------------------------------------
# 第三方依赖
# ---------------------------------------------------------------------------
# 依赖默认从 GitHub 直接下载。网络受限时，请由使用者在本机配置代理，不要在
# CMake 中硬编码镜像或离线缓存，例如：
#   export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890
#   git config --global http.proxy http://127.0.0.1:7890

# nlohmann/json
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Saucer v8.x（WebView 后端）
FetchContent_Declare(saucer
    URL https://github.com/saucer/saucer/archive/refs/tags/v8.0.5.tar.gz
    URL_HASH SHA256=d0f1bd3bdbbc37c2e15917c92a100400443eefec730564585d90e24fd632a03c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# saucer-embed v1.1.1 在 CMake < 3.31 上使用了尚未定义的 CMP0174 策略，
# 这里通过 CMAKE_PROJECT_INCLUDE 自动加保护（仅影响旧版 CMake）。
if(CMAKE_VERSION VERSION_LESS 3.31)
    set(CMAKE_PROJECT_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/saucer_cmp0174_compat.cmake")
endif()

FetchContent_MakeAvailable(saucer nlohmann_json)

# ---------------------------------------------------------------------------
# whiz 库
# ---------------------------------------------------------------------------

include(cmake/whiz_embed.cmake)

add_library(whiz STATIC
    src/application.cpp
    src/web_window.cpp
    src/js_bridge.cpp
    src/fs_bridge.cpp
    src/dialog_bridge.cpp
)
add_library(whiz::whiz ALIAS whiz)

target_compile_features(whiz PUBLIC cxx_std_23)
set_target_properties(whiz PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(whiz PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(whiz
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE saucer::saucer
)

# dialog_bridge.cpp 在 Linux 上直接使用 GTK4 原生对话框 API（文档 8.5.3），
# 并通过 saucer 的 stable_natives 获取 GtkWindow*，需要相关头文件。
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
    pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET webkitgtk-6.0)
    target_link_libraries(whiz PRIVATE PkgConfig::GTK4 PkgConfig::LIBADWAITA PkgConfig::WEBKITGTK)
endif()
