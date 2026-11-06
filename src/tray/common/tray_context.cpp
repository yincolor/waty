// TrayContext 公共实现：平台初始化的幂等包装。
//
// 平台初始化/清理是进程级的，但 API 允许创建多个 TrayContext。
// 这里的 initialized 仅代表本对象是否调用过 platform_initialize()；
// 真正的连接由各平台实现共享（Linux 为全局 D-Bus 连接）。
// 注意：任一 TrayContext 析构都会触发 platform_cleanup()，
// 因此同一进程内不建议同时持有多个 TrayContext。

#include <waty/tray.hpp>

#include "../platform.hpp"

#include <memory>

namespace waty::tray
{
    struct TrayContext::Impl
    {
        bool initialized = false;
    };

    TrayContext::TrayContext()
        : m_impl(std::make_unique<Impl>())
    {
    }

    TrayContext::~TrayContext()
    {
        if (m_impl && m_impl->initialized)
        {
            detail::platform_cleanup();
        }
    }

    TrayResult<void> TrayContext::initialize()
    {
        if (m_impl->initialized)
        {
            return {};
        }

        auto result = detail::platform_initialize();
        if (result)
        {
            m_impl->initialized = true;
        }
        return result;
    }
} // namespace waty::tray
