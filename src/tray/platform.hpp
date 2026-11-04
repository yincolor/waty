#pragma once

// waty-tray 内部共享头文件：平台后端抽象（仅 src/ 内部使用）。

#include <waty/tray.hpp>

#include <memory>

namespace waty::tray::detail
{
    // 由各平台后端（linux / windows / macos）实现：
    TrayResult<void> platform_initialize();
    void platform_cleanup();
    TrayResult<std::unique_ptr<TrayIcon>> platform_create_tray_icon();
} // namespace waty::tray::detail
