# ---------------------------------------------------------------------------
# waty-notify 内部模块
#
# 平台后端抽象见 backend.hpp；各平台后端实现：
#   - Windows: WinRT Toast（backend_windows.cpp）
#   - macOS:   UserNotifications（backend_macos.mm）
#   - Linux:   D-Bus 通知（backend_linux.cpp）
#
# 使用 OBJECT 库：对象文件并入顶层 waty 静态库，避免私有静态库传递链接问题。
# ---------------------------------------------------------------------------

add_library(waty_notify OBJECT
    api.cpp
    event_loop.cpp
)

target_compile_features(waty_notify PUBLIC cxx_std_23)
set_target_properties(waty_notify PROPERTIES CXX_EXTENSIONS OFF)

# 访问 <waty/notify.hpp> 等公共头
target_include_directories(waty_notify PRIVATE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

if(WIN32)
    target_sources(waty_notify PRIVATE backend_windows.cpp)
    target_link_libraries(waty_notify PRIVATE windowsapp)
elseif(APPLE)
    target_sources(waty_notify PRIVATE backend_macos.mm)
    target_link_libraries(waty_notify PRIVATE
        "-framework UserNotifications"
        "-framework Foundation")
else()
    target_sources(waty_notify PRIVATE backend_linux.cpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    target_link_libraries(waty_notify PRIVATE PkgConfig::DBUS)
endif()
