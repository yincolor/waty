cmake_minimum_required(VERSION 3.25)

project(waty LANGUAGES CXX VERSION 0.1.0)

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

# SQLite（sjinks/sqlite3-cmake，v3.49.1）
# 注意：该包默认 sqlite3_ENABLE_THREADSAFE=OFF → 注入 SQLITE_THREADSAFE=0。
# waty 会在“桥接线程打开 / 工作线程执行 / 任意线程关闭”之间传递连接，
# 因此必须显式开启线程安全（SQLITE_THREADSAFE=1，serialized）。
set(sqlite3_ENABLE_THREADSAFE ON CACHE BOOL "" FORCE)
FetchContent_Declare(sqlite3
    URL https://github.com/sjinks/sqlite3-cmake/archive/42e4aad2da70e5be84349915882a117742b042c2.tar.gz
    URL_HASH SHA256=3e6adbec688e85b10500d7303a05edd415b622415cea48a7538fd99ff4d9bbe0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(CMAKE_VERSION VERSION_LESS 3.31)
    set(CMAKE_PROJECT_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/saucer_cmp0174_compat.cmake")
endif()

FetchContent_MakeAvailable(saucer nlohmann_json sqlite3)

# ---------------------------------------------------------------------------
# WebKitGTK 2.54 兼容
# ---------------------------------------------------------------------------
# WebKitGTK 2.54 起 <webkit/webkit.h> 把 WebKitPointerLockPermissionRequest.h
# 包在 `#if defined(ENABLE_POINTER_LOCK) && ENABLE_POINTER_LOCK` 中，但 pkg-config
# 不会导出该构建宏，导致 saucer 8.0.5 引用
# WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST 时编译失败。
# GTK 端口默认启用 pointer lock 且库导出对应符号，因此仅为 saucer 目标补上该宏。
# 对旧版 WebKit（无条件包含该头文件）及修复后的版本（324124@main）均为无害 no-op。
# 参考：https://bugs.webkit.org/show_bug.cgi?id=328048
if(UNIX AND NOT APPLE)
    target_compile_definitions(saucer PRIVATE ENABLE_POINTER_LOCK=1)
endif()

# ---------------------------------------------------------------------------
# waty 库
# ---------------------------------------------------------------------------

include(cmake/waty_embed.cmake)

add_library(waty STATIC
    src/application.cpp
    src/paths.cpp
    src/web_window.cpp
    src/js_bridge.cpp
    src/fs_bridge.cpp
    src/base64.cpp
    src/dialog_bridge.cpp
    src/sqlite3_database.cpp
    src/sqlite3_service.cpp
    src/sqlite3_bridge.cpp
    # notify 内部模块
    src/notify_bridge.cpp
    src/notify/api.cpp
    src/notify/event_loop.cpp
    # tray 内部模块（公共部分）
    src/tray/common/tray_context.cpp
    src/tray/common/tray_icon.cpp
    src/tray/common/menu.cpp
)
add_library(waty::waty ALIAS waty)

target_compile_features(waty PUBLIC cxx_std_23)
set_target_properties(waty PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(waty PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(waty
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE saucer::saucer
            SQLite::SQLite3
)

# dialog_bridge.cpp 在 Linux 上直接使用 GTK4 原生对话框 API（文档 8.5.3），
# 并通过 saucer 的 stable_natives 获取 GtkWindow*，需要相关头文件。
#
# notify / tray 内部模块按平台追加后端源文件与系统依赖：
#   - notify:  Windows WinRT Toast / macOS UserNotifications / Linux D-Bus
#   - tray:    Windows Shell_NotifyIcon(占位) / macOS NSStatusItem(占位) / Linux SNI D-Bus
if(WIN32)
    target_sources(waty PRIVATE
        src/notify/backend_windows.cpp
        src/tray/windows/tray_icon_win.cpp
        src/tray/windows/tray_context_win.cpp
    )
    target_link_libraries(waty PRIVATE windowsapp shell32 user32)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(waty PRIVATE
        src/notify/backend_macos.mm
        src/tray/macos/tray_icon_mac.mm
        src/tray/macos/tray_context_mac.mm
    )
    target_link_libraries(waty PRIVATE
        "-framework UserNotifications"
        "-framework Foundation"
        "-framework Cocoa")
else()
    target_sources(waty PRIVATE
        src/notify/backend_linux.cpp
        src/tray/linux/tray_icon_linux.cpp
        src/tray/linux/tray_context_linux.cpp
        src/tray/linux/sni_dbus.cpp
    )
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
    pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET webkitgtk-6.0)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
    pkg_check_modules(GDKPIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
    target_link_libraries(waty PRIVATE
        PkgConfig::GTK4 PkgConfig::LIBADWAITA PkgConfig::WEBKITGTK
        PkgConfig::DBUS PkgConfig::GLIB PkgConfig::GDKPIXBUF)
endif()

# ---------------------------------------------------------------------------
# 单测（默认关闭；不依赖 WebView，仅链接 sqlite3）
# ---------------------------------------------------------------------------
option(WATY_BUILD_TESTS "Build waty unit tests" OFF)
if(WATY_BUILD_TESTS)
    enable_testing()
    add_executable(waty_sqlite3_tests
        tests/sqlite3_tests.cpp
        src/base64.cpp
        src/sqlite3_database.cpp
        src/sqlite3_service.cpp
    )
    target_compile_features(waty_sqlite3_tests PRIVATE cxx_std_23)
    target_include_directories(waty_sqlite3_tests PRIVATE src)
    target_link_libraries(waty_sqlite3_tests PRIVATE SQLite::SQLite3 nlohmann_json::nlohmann_json)
    add_test(NAME sqlite3 COMMAND waty_sqlite3_tests)
endif()
