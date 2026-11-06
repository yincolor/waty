// macOS 托盘上下文后端（占位实现）。
// 计划：确保 NSApplication 已初始化，并持有进程级 NSStatusBar。
// 当前返回 PlatformNotSupported。

#include "../platform.hpp"

#include <waty/tray.hpp>

#include <memory>

namespace waty::tray::detail
{
    TrayResult<void> platform_initialize()
    {
        // TODO(platform): macOS 后端（NSStatusBar / NSStatusItem）
        return std::unexpected(TrayError::PlatformNotSupported);
    }

    void platform_cleanup()
    {
        // TODO(platform): macOS 后端
    }
} // namespace waty::tray::detail
