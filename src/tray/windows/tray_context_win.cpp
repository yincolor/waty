// Windows 托盘上下文后端（占位实现）。
// 计划：注册隐藏窗口类并创建一个消息窗口，供 Shell_NotifyIcon 回调使用。
// 当前返回 PlatformNotSupported。

#include "../platform.hpp"

#include <waty/tray.hpp>

#include <memory>

namespace waty::tray::detail
{
    TrayResult<void> platform_initialize()
    {
        // TODO(platform): Windows 后端（Shell_NotifyIcon + 隐藏消息窗口）
        return std::unexpected(TrayError::PlatformNotSupported);
    }

    void platform_cleanup()
    {
        // TODO(platform): Windows 后端
    }
} // namespace waty::tray::detail
