# ---------------------------------------------------------------------------
# waty-tray 内部模块
#
# 平台后端抽象见 platform.hpp；各平台后端实现：
#   - Windows: Shell_NotifyIcon（占位，待实现）
#   - macOS:   NSStatusItem（占位，待实现）
#   - Linux:   StatusNotifierItem / D-Bus（linux/）
#
# 使用 OBJECT 库：对象文件并入顶层 waty 静态库，避免私有静态库传递链接问题。
# ---------------------------------------------------------------------------

add_library(waty_tray OBJECT
    common/tray_context.cpp
    common/tray_icon.cpp
    common/menu.cpp
)

target_compile_features(waty_tray PUBLIC cxx_std_23)
set_target_properties(waty_tray PROPERTIES CXX_EXTENSIONS OFF)

# 访问 <waty/tray.hpp> 等公共头
target_include_directories(waty_tray PRIVATE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

if(WIN32)
    target_sources(waty_tray PRIVATE
        windows/tray_icon_win.cpp
        windows/tray_context_win.cpp
    )
elseif(APPLE)
    target_sources(waty_tray PRIVATE
        macos/tray_icon_mac.mm
        macos/tray_context_mac.mm
    )
    target_link_libraries(waty_tray PRIVATE "-framework Cocoa")
else()
    target_sources(waty_tray PRIVATE
        linux/tray_icon_linux.cpp
        linux/tray_context_linux.cpp
        linux/sni_dbus.cpp
    )
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
    pkg_check_modules(GDKPIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
    target_link_libraries(waty_tray PRIVATE
        PkgConfig::DBUS
        PkgConfig::GLIB
        PkgConfig::GDKPIXBUF)
endif()
