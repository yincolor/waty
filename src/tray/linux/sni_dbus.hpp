#pragma once

// Linux SNI D-Bus 后端内部声明（仅 src/linux/ 使用）。

#include <dbus/dbus.h>

#include <waty/tray.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace waty::tray::detail
{
    /// 共享会话总线连接（由 TrayContext::initialize 或首次创建托盘时初始化）。
    DBusConnection *shared_connection();

    /// 单个 StatusNotifierItem + com.canonical.dbusmenu 实现。
    class SniItem
    {
      public:
        SniItem(DBusConnection *conn, std::string id, std::string title);
        ~SniItem();

        SniItem(const SniItem &)            = delete;
        SniItem &operator=(const SniItem &) = delete;

        /// 请求服务名、注册对象路径并向 StatusNotifierWatcher 注册。
        TrayResult<void> register_with_watcher();

        void set_title(std::string title);
        void set_status(std::string status);
        void set_icon_name(std::string name);
        void set_icon_pixmap(std::vector<std::byte> rgba, std::uint32_t width, std::uint32_t height);
        void set_tooltip(std::string title, std::string description);
        void set_menu(const Menu *menu);

        // 事件回调（在 UI 线程执行）
        std::move_only_function<void()> on_activate;
        std::move_only_function<void()> on_secondary_activate;
        std::move_only_function<void()> on_scroll;

        [[nodiscard]] const std::string &service_name() const noexcept
        {
            return service_name_;
        }

        [[nodiscard]] const std::string &object_path() const noexcept
        {
            return object_path_;
        }

        /// D-Bus 对象路径消息处理入口。
        DBusHandlerResult handle_message(DBusMessage *message);

      private:
        struct FlatItem
        {
            int id = 0;
            const MenuItem *item = nullptr;
            std::vector<int> children;
        };

        DBusHandlerResult handle_properties(DBusMessage *msg);
        DBusHandlerResult handle_sni(DBusMessage *msg);
        DBusHandlerResult handle_dbusmenu(DBusMessage *msg);

        void rebuild_menu_layout();
        bool append_sni_property(DBusMessageIter *dict, const char *name);
        void append_sni_value(DBusMessageIter *variant, const std::string &key);
        bool append_menu_property(DBusMessageIter *dict, const FlatItem &item, const char *name);
        bool append_layout_item(DBusMessageIter *array, int id);

        DBusConnection *conn_ = nullptr;
        std::string service_name_;
        std::string object_path_ = "/StatusNotifierItem";
        std::string menu_path_   = "/MenuBar";
        std::string id_;
        std::string title_;
        std::string status_ = "Active";
        std::string icon_name_;
        std::string tooltip_title_;
        std::string tooltip_description_;
        std::vector<std::byte> icon_pixels_;
        std::uint32_t icon_w_ = 0;
        std::uint32_t icon_h_ = 0;

        const Menu *menu_ = nullptr;
        std::unordered_map<int, FlatItem> flat_;
        std::vector<int> root_children_;
        std::uint32_t layout_revision_ = 0;
        bool registered_ = false;
    };
} // namespace waty::tray::detail
