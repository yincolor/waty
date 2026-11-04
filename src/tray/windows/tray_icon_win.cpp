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
