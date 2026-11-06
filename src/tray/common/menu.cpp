// 托盘菜单容器实现。
//
// Menu 只是 MenuItem 的有序集合；没有平台依赖，也不做校验。
// 真正校验/渲染发生在 TrayIcon::set_menu() → 各平台后端。
// MenuItem 含 move_only_function（on_click），因此 Menu 只能移动不能拷贝；
// 传递时请用 std::move。

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
