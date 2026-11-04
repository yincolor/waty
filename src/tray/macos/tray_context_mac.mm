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
