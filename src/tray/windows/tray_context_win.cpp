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
