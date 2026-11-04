#include <waty/tray.hpp>

#include "../platform.hpp"

#include <memory>

namespace waty::tray
{
    TrayResult<std::unique_ptr<TrayIcon>> TrayIcon::create()
    {
        return detail::platform_create_tray_icon();
    }
} // namespace waty::tray
