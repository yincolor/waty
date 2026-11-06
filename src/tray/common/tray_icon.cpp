// TrayIcon 公共工厂：直接转发给平台后端。
//
// 返回的具体类型由编译目标决定（Linux: LinuxTrayIcon；Windows/macOS 暂为未实现）。
// 调用方只依赖 TrayIcon 接口，不感知平台实现。

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
