#include <waty/tray.hpp>

#include <utility>

namespace waty::tray
{
    Menu::Menu(std::vector<MenuItem> items)
        : m_items(std::move(items))
    {
    }

    void Menu::add_item(MenuItem item)
    {
        m_items.push_back(std::move(item));
    }

    void Menu::clear()
    {
        m_items.clear();
    }

    const std::vector<MenuItem> &Menu::items() const noexcept
    {
        return m_items;
    }
} // namespace waty::tray
