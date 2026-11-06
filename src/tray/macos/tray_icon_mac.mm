// macOS 托盘图标后端（占位实现）。
// 计划：NSStatusBar + NSStatusItem，菜单用 NSMenu。
// 当前返回 PlatformNotSupported。

#include "../platform.hpp"

#include <waty/tray.hpp>

#include <memory>

namespace waty::tray::detail
{
    TrayResult<std::unique_ptr<TrayIcon>> platform_create_tray_icon()
    {
        // TODO(platform): macOS 后端（NSStatusItem）
        return std::unexpected(TrayError::PlatformNotSupported);
    }
} // namespace waty::tray::detail
