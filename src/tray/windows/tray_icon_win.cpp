// Windows 托盘图标后端（占位实现）。
// 计划：Shell_NotifyIcon + 隐藏消息窗口接收 WM_* 回调。
// 当前返回 PlatformNotSupported，公共 API 可跨平台调用但 Windows 上不可用。

#include "../platform.hpp"

#include <waty/tray.hpp>

#include <memory>

namespace waty::tray::detail
{
    TrayResult<std::unique_ptr<TrayIcon>> platform_create_tray_icon()
    {
        // TODO(platform): Windows 后端（Shell_NotifyIcon）
        return std::unexpected(TrayError::PlatformNotSupported);
    }
} // namespace waty::tray::detail
