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
