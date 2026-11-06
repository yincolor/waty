#pragma once

// waty-tray 内部共享头文件：平台后端抽象（仅 src/ 内部使用）。
//
// 每个平台目录实现三个函数：
//   * platform_initialize()  —— 建立进程级资源（Linux: 共享 D-Bus 连接）；
//   * platform_cleanup()     —— 释放上述资源（幂等）；
//   * platform_create_tray_icon() —— 创建具体 TrayIcon（无宿主时返回 NoTrayHost）。
//
// 未实现的后端返回 TrayError::PlatformNotSupported。

#include <waty/tray.hpp>

#include <memory>

namespace waty::tray::detail
{
    // 由各平台后端（linux / windows / macos）实现：
    TrayResult<void> platform_initialize();
    void platform_cleanup();
    TrayResult<std::unique_ptr<TrayIcon>> platform_create_tray_icon();
} // namespace waty::tray::detail
