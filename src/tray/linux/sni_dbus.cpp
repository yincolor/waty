#include "sni_dbus.hpp"

#include <dbus/dbus.h>

#include <cstring>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include <unistd.h>

namespace waty::tray::detail
{
    namespace
    {
        DBusHandlerResult message_cb(DBusConnection *, DBusMessage *message, void *user_data)
        {
            if (!message || !user_data)
            {
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }
            return static_cast<SniItem *>(user_data)->handle_message(message);
        }

        void unregister_cb(DBusConnection *, void *)
        {
        }

        const DBusObjectPathVTable k_vtable = {
            .unregister_function = unregister_cb,
            .message_function   = message_cb,
        };

        void append_dict_entry_string(DBusMessageIter *dict, const char *key, const char *value)
        {
            DBusMessageIter entry, variant;
            dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(dict, &entry);
        }

        void append_dict_entry_bool(DBusMessageIter *dict, const char *key, bool value)
        {
            DBusMessageIter entry, variant;
            dbus_bool_t v = value ? TRUE : FALSE;
            dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(dict, &entry);
        }

        void append_dict_entry_int32(DBusMessageIter *dict, const char *key, std::int32_t value)
        {
            DBusMessageIter entry, variant;
            dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(dict, &entry);
        }
    } // namespace

    SniItem::SniItem(DBusConnection *conn, std::string id, std::string title)
        : conn_(conn), id_(std::move(id)), title_(std::move(title))
    {
        service_name_ = "org.kde.StatusNotifierItem-" + std::to_string(static_cast<long long>(::getpid())) + "-" + id_;
    }

    SniItem::~SniItem()
    {
        if (registered_ && conn_)
        {
            dbus_connection_unregister_object_path(conn_, object_path_.c_str());
            if (!menu_path_.empty())
            {
                dbus_connection_unregister_object_path(conn_, menu_path_.c_str());
            }
        }
    }

    TrayResult<void> SniItem::register_with_watcher()
    {
        if (!conn_)
        {
            return std::unexpected(TrayError::DbusConnectionFailed);
        }

        DBusError error;
        dbus_error_init(&error);

        // 请求服务名
        const int name_reply = dbus_bus_request_name(conn_, service_name_.c_str(),
                                                     DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
        if (dbus_error_is_set(&error) || name_reply == DBUS_REQUEST_NAME_REPLY_EXISTS)
        {
            dbus_error_free(&error);
            return std::unexpected(TrayError::DbusConnectionFailed);
        }

        // 注册对象路径
        if (!dbus_connection_register_object_path(conn_, object_path_.c_str(), &k_vtable, this))
        {
            dbus_error_free(&error);
            return std::unexpected(TrayError::NativeApiFailed);
        }

        // 菜单对象路径（挂到同一 vtable，通过 user_data 分发）
        if (!dbus_connection_register_object_path(conn_, menu_path_.c_str(), &k_vtable, this))
        {
            dbus_connection_unregister_object_path(conn_, object_path_.c_str());
            dbus_error_free(&error);
            return std::unexpected(TrayError::NativeApiFailed);
        }

        // 向 StatusNotifierWatcher 注册
        DBusMessage *msg = dbus_message_new_method_call(
            "org.kde.StatusNotifierWatcher",
            "/StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher",
            "RegisterStatusNotifierItem");
        if (!msg)
        {
            dbus_error_free(&error);
            return std::unexpected(TrayError::NativeApiFailed);
        }

        const char *service = service_name_.c_str();
        dbus_message_append_args(msg, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, 1000, &error);
        dbus_message_unref(msg);

        if (reply)
        {
            dbus_message_unref(reply);
        }

        if (dbus_error_is_set(&error))
        {
            dbus_error_free(&error);
            dbus_connection_unregister_object_path(conn_, object_path_.c_str());
            dbus_connection_unregister_object_path(conn_, menu_path_.c_str());
            return std::unexpected(TrayError::DbusConnectionFailed);
        }

        registered_ = true;
        return {};
    }

    void SniItem::set_title(std::string title)
    {
        title_ = std::move(title);
    }

    void SniItem::set_status(std::string status)
    {
        status_ = std::move(status);
    }

    void SniItem::set_icon_name(std::string name)
    {
        icon_name_ = std::move(name);
        icon_pixels_.clear();
        icon_w_ = 0;
        icon_h_ = 0;
    }

    void SniItem::set_icon_pixmap(std::vector<std::byte> rgba, std::uint32_t width, std::uint32_t height)
    {
        icon_name_.clear();
        icon_w_ = width;
        icon_h_ = height;
        icon_pixels_.clear();
        icon_pixels_.reserve(static_cast<std::size_t>(width) * height * 4);
        // RGBA → ARGB32（SNI IconPixmap 格式）
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            icon_pixels_.push_back(rgba[i + 3]);  // A
            icon_pixels_.push_back(rgba[i + 0]);  // R
            icon_pixels_.push_back(rgba[i + 1]);  // G
            icon_pixels_.push_back(rgba[i + 2]);  // B
        }
    }

    void SniItem::set_tooltip(std::string title, std::string description)
    {
        tooltip_title_       = std::move(title);
        tooltip_description_ = std::move(description);
    }

    void SniItem::set_menu(const Menu *menu)
    {
        menu_ = menu;
        rebuild_menu_layout();
    }

    void SniItem::rebuild_menu_layout()
    {
        flat_.clear();
        root_children_.clear();

        if (!menu_)
        {
            layout_revision_++;
            return;
        }

        int next_id = 1;
        std::function<void(const std::vector<MenuItem> &, std::vector<int> &)> walk =
            [&](const std::vector<MenuItem> &items, std::vector<int> &out) {
                for (const auto &item : items)
                {
                    const int id = next_id++;
                    FlatItem flat;
                    flat.id   = id;
                    flat.item = &item;
                    if (item.type == MenuItemType::Submenu)
                    {
                        walk(item.children, flat.children);
                    }
                    out.push_back(id);
                    flat_[id] = std::move(flat);

                    std::fprintf(stderr, "[waty-tray] menu item: id=%d label=%s type=%d\n",
                                 id, item.label.c_str(), static_cast<int>(item.type));
                    std::fflush(stderr);
                }
            };

        walk(menu_->items(), root_children_);
        layout_revision_++;
    }

    DBusHandlerResult SniItem::handle_message(DBusMessage *message)
    {
        if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Get") ||
            dbus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "GetAll") ||
            dbus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Set"))
        {
            return handle_properties(message);
        }
        if (dbus_message_has_interface(message, "org.kde.StatusNotifierItem"))
        {
            return handle_sni(message);
        }
        if (dbus_message_has_interface(message, "com.canonical.dbusmenu"))
        {
            return handle_dbusmenu(message);
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    void SniItem::append_sni_value(DBusMessageIter *variant, const std::string &key)
    {
        if (key == "Category")
        {
            const char *v = "ApplicationStatus";
            dbus_message_iter_append_basic(variant, DBUS_TYPE_STRING, &v);
        }
        else if (key == "Id")
        {
            const char *v = id_.c_str();
            dbus_message_iter_append_basic(variant, DBUS_TYPE_STRING, &v);
        }
        else if (key == "Title")
        {
            const char *v = title_.c_str();
            dbus_message_iter_append_basic(variant, DBUS_TYPE_STRING, &v);
        }
        else if (key == "Status")
        {
            const char *v = status_.c_str();
            dbus_message_iter_append_basic(variant, DBUS_TYPE_STRING, &v);
        }
        else if (key == "IconName")
        {
            const char *v = icon_name_.c_str();
            dbus_message_iter_append_basic(variant, DBUS_TYPE_STRING, &v);
        }
        else if (key == "Menu")
        {
            const char *v = menu_ ? menu_path_.c_str() : "/NO_DBUSMENU";
            dbus_message_iter_append_basic(variant, DBUS_TYPE_OBJECT_PATH, &v);
        }
        else if (key == "IconPixmap")
        {
            // a(iiay)
            DBusMessageIter arr, str, bytes;
            dbus_message_iter_open_container(variant, DBUS_TYPE_ARRAY, "(iiay)", &arr);
            dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &str);
            dbus_message_iter_append_basic(&str, DBUS_TYPE_INT32, &icon_w_);
            dbus_message_iter_append_basic(&str, DBUS_TYPE_INT32, &icon_h_);
            dbus_message_iter_open_container(&str, DBUS_TYPE_ARRAY, "y", &bytes);
            for (const auto byte : icon_pixels_)
            {
                const unsigned char b = static_cast<unsigned char>(byte);
                dbus_message_iter_append_basic(&bytes, DBUS_TYPE_BYTE, &b);
            }
            dbus_message_iter_close_container(&str, &bytes);
            dbus_message_iter_close_container(&arr, &str);
            dbus_message_iter_close_container(variant, &arr);
        }
        else if (key == "ToolTip")
        {
            // (sa(iiay)ss)
            const char *icon  = "";
            const char *title = tooltip_title_.c_str();
            const char *desc  = tooltip_description_.c_str();
            DBusMessageIter str, sub;
            dbus_message_iter_open_container(variant, DBUS_TYPE_STRUCT, nullptr, &str);
            dbus_message_iter_append_basic(&str, DBUS_TYPE_STRING, &icon);
            dbus_message_iter_open_container(&str, DBUS_TYPE_ARRAY, "(iiay)", &sub);
            dbus_message_iter_close_container(&str, &sub);
            dbus_message_iter_append_basic(&str, DBUS_TYPE_STRING, &title);
            dbus_message_iter_append_basic(&str, DBUS_TYPE_STRING, &desc);
            dbus_message_iter_close_container(variant, &str);
        }
        else
        {
            const char *v = "";
            dbus_message_iter_append_basic(variant, DBUS_TYPE_STRING, &v);
        }
    }

    bool SniItem::append_sni_property(DBusMessageIter *dict, const char *name)
    {
        const std::string key(name);

        // 确定属性的 D-Bus 类型签名
        const char *signature = "s";
        if (key == "Menu") signature = "o";
        else if (key == "IconPixmap") signature = "a(iiay)";
        else if (key == "ToolTip") signature = "(sa(iiay)ss)";

        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant);
        append_sni_value(&variant, key);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(dict, &entry);
        return true;
    }

    DBusHandlerResult SniItem::handle_properties(DBusMessage *msg)
    {
        if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get"))
        {
            const char *interface = nullptr;
            const char *property  = nullptr;
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &interface,
                                       DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID))
            {
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }

            const std::string key(property ? property : "");
            const char *signature = "s";
            if (key == "Menu") signature = "o";
            else if (key == "IconPixmap") signature = "a(iiay)";
            else if (key == "ToolTip") signature = "(sa(iiay)ss)";

            DBusMessage *reply = dbus_message_new_method_return(msg);
            DBusMessageIter iter, variant;
            dbus_message_iter_init_append(reply, &iter);
            dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, signature, &variant);
            append_sni_value(&variant, key);
            dbus_message_iter_close_container(&iter, &variant);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "GetAll"))
        {
            const char *interface = nullptr;
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID))
            {
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }

            DBusMessage *reply = dbus_message_new_method_return(msg);
            DBusMessageIter iter, dict;
            dbus_message_iter_init_append(reply, &iter);
            dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

            if (std::strcmp(interface, "org.kde.StatusNotifierItem") == 0)
            {
                append_sni_property(&dict, "Category");
                append_sni_property(&dict, "Id");
                append_sni_property(&dict, "Title");
                append_sni_property(&dict, "Status");
                append_sni_property(&dict, "IconName");
                append_sni_property(&dict, "IconPixmap");
                append_sni_property(&dict, "ToolTip");
                append_sni_property(&dict, "Menu");
            }
            else if (std::strcmp(interface, "com.canonical.dbusmenu") == 0)
            {
                const std::uint32_t version = 3;
                const char *direction = "ltr";
                const char *status = "normal";
                const char *version_key = "Version";
                const char *direction_key = "TextDirection";
                const char *status_key = "Status";
                DBusMessageIter entry, variant;

                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &version_key);
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &version);
                dbus_message_iter_close_container(&entry, &variant);
                dbus_message_iter_close_container(&dict, &entry);

                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &direction_key);
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &direction);
                dbus_message_iter_close_container(&entry, &variant);
                dbus_message_iter_close_container(&dict, &entry);

                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &status_key);
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &status);
                dbus_message_iter_close_container(&entry, &variant);
                dbus_message_iter_close_container(&dict, &entry);
            }

            dbus_message_iter_close_container(&iter, &dict);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        // Set：只读，返回错误
        if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Set"))
        {
            DBusMessage *reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Error.PropertyReadOnly", "read only");
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusHandlerResult SniItem::handle_sni(DBusMessage *msg)
    {
        const char *member = dbus_message_get_member(msg);
        std::fprintf(stderr, "[waty-tray] handle_sni: member=%s\n", member ? member : "(null)");
        std::fflush(stderr);

        DBusMessage *reply = dbus_message_new_method_return(msg);

        if (dbus_message_is_method_call(msg, "org.kde.StatusNotifierItem", "Activate"))
        {
            if (on_activate)
            {
                on_activate();
            }
        }
        else if (dbus_message_is_method_call(msg, "org.kde.StatusNotifierItem", "SecondaryActivate"))
        {
            if (on_secondary_activate)
            {
                on_secondary_activate();
            }
        }
        else if (dbus_message_is_method_call(msg, "org.kde.StatusNotifierItem", "ContextMenu"))
        {
            // 上下文菜单由宿主根据 Menu 属性（dbusmenu）自行渲染，无需回调。
        }
        else if (dbus_message_is_method_call(msg, "org.kde.StatusNotifierItem", "Scroll"))
        {
            if (on_scroll)
            {
                on_scroll();
            }
        }
        else
        {
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        dbus_connection_send(conn_, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    bool SniItem::append_menu_property(DBusMessageIter *dict, const FlatItem &item, const char *name)
    {
        const std::string key(name);
        if (!item.item)
        {
            return false;
        }

        if (key == "label")
        {
            const char *v = item.item->label.c_str();
            append_dict_entry_string(dict, name, v);
            return true;
        }
        if (key == "enabled")
        {
            append_dict_entry_bool(dict, name, item.item->enabled);
            return true;
        }
        if (key == "visible")
        {
            append_dict_entry_bool(dict, name, true);
            return true;
        }
        if (key == "type")
        {
            const char *v = (item.item->type == MenuItemType::Separator) ? "separator" : "standard";
            append_dict_entry_string(dict, name, v);
            return true;
        }
        if (key == "toggle-type")
        {
            const char *v = "checkmark";
            append_dict_entry_string(dict, name, v);
            return true;
        }
        if (key == "toggle-state")
        {
            append_dict_entry_int32(dict, name, item.item->checked ? 1 : 0);
            return true;
        }
        if (key == "children-display")
        {
            const char *v = "submenu";
            append_dict_entry_string(dict, name, v);
            return true;
        }
        return false;
    }

    bool SniItem::append_layout_item(DBusMessageIter *parent, int id)
    {
        const auto it = flat_.find(id);
        if (it == flat_.end())
        {
            return false;
        }

        DBusMessageIter item_struct, props, children_arr, child_variant;
        dbus_message_iter_open_container(parent, DBUS_TYPE_STRUCT, nullptr, &item_struct);
        dbus_message_iter_append_basic(&item_struct, DBUS_TYPE_INT32, &id);

        dbus_message_iter_open_container(&item_struct, DBUS_TYPE_ARRAY, "{sv}", &props);
        append_menu_property(&props, it->second, "label");
        append_menu_property(&props, it->second, "enabled");
        append_menu_property(&props, it->second, "visible");
        append_menu_property(&props, it->second, "type");
        if (it->second.item->type == MenuItemType::Checkbox)
        {
            append_menu_property(&props, it->second, "toggle-type");
            append_menu_property(&props, it->second, "toggle-state");
        }
        if (it->second.item->type == MenuItemType::Submenu)
        {
            append_menu_property(&props, it->second, "children-display");
        }
        dbus_message_iter_close_container(&item_struct, &props);

        // 子菜单：av（variant 数组，每个 variant 包裹一个 (ia{sv}av) 项）
        dbus_message_iter_open_container(&item_struct, DBUS_TYPE_ARRAY, "v", &children_arr);
        for (const int child : it->second.children)
        {
            dbus_message_iter_open_container(&children_arr, DBUS_TYPE_VARIANT, "(ia{sv}av)", &child_variant);
            append_layout_item(&child_variant, child);
            dbus_message_iter_close_container(&children_arr, &child_variant);
        }
        dbus_message_iter_close_container(&item_struct, &children_arr);

        dbus_message_iter_close_container(parent, &item_struct);
        return true;
    }

    DBusHandlerResult SniItem::handle_dbusmenu(DBusMessage *msg)
    {
        const char *member = dbus_message_get_member(msg);
        std::fprintf(stderr, "[waty-tray] handle_dbusmenu: member=%s\n", member ? member : "(null)");
        std::fflush(stderr);

        if (dbus_message_is_method_call(msg, "com.canonical.dbusmenu", "GetLayout"))
        {
            int parent_id = 0;
            int recursion = -1;
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_INT32, &parent_id,
                                       DBUS_TYPE_INT32, &recursion, DBUS_TYPE_INVALID))
            {
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }

            DBusMessage *reply = dbus_message_new_method_return(msg);
            DBusMessageIter iter, layout, props, children_arr;
            dbus_message_iter_init_append(reply, &iter);

            // 返回签名：u (ia{sv}av) —— 修订号 + 请求节点的布局结构（与 dbusmenu 客户端预期一致）。
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &layout_revision_);

            dbus_message_iter_open_container(&iter, DBUS_TYPE_STRUCT, nullptr, &layout);
            dbus_message_iter_append_basic(&layout, DBUS_TYPE_INT32, &parent_id);

            // 布局结构自身的属性（根/父节点为空字典）
            dbus_message_iter_open_container(&layout, DBUS_TYPE_ARRAY, "{sv}", &props);
            dbus_message_iter_close_container(&layout, &props);

            // 子项：av（每个 variant 包裹一个 (ia{sv}av) 项）
            const std::vector<int> *children = nullptr;
            if (parent_id == 0)
            {
                children = &root_children_;
            }
            else
            {
                const auto it = flat_.find(parent_id);
                if (it != flat_.end())
                {
                    children = &it->second.children;
                }
            }

            dbus_message_iter_open_container(&layout, DBUS_TYPE_ARRAY, "v", &children_arr);
            if (children != nullptr)
            {
                for (const int child : *children)
                {
                    DBusMessageIter child_variant;
                    dbus_message_iter_open_container(&children_arr, DBUS_TYPE_VARIANT, "(ia{sv}av)", &child_variant);
                    append_layout_item(&child_variant, child);
                    dbus_message_iter_close_container(&children_arr, &child_variant);
                }
            }
            dbus_message_iter_close_container(&layout, &children_arr);

            dbus_message_iter_close_container(&iter, &layout);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_is_method_call(msg, "com.canonical.dbusmenu", "Event"))
        {
            int id = 0;
            const char *event_id = nullptr;
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_INT32, &id,
                                       DBUS_TYPE_STRING, &event_id, DBUS_TYPE_INVALID))
            {
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }

            std::fprintf(stderr, "[waty-tray] dbusmenu Event: id=%d event_id=%s\n",
                         id, event_id ? event_id : "(null)");
            std::fflush(stderr);

            if (event_id && std::strcmp(event_id, "clicked") == 0)
            {
                const auto it = flat_.find(id);
                if (it != flat_.end() && it->second.item && it->second.item->on_click)
                {
                    // Menu 实际由 TrayIcon 以非 const 持有；这里仅因 Menu::items() 返回
                    // const 引用而需要 const_cast，以调用 move_only_function（其 operator() 非 const）。
                    const_cast<MenuItem *>(it->second.item)->on_click();
                }
            }

            DBusMessage *reply = dbus_message_new_method_return(msg);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_is_method_call(msg, "com.canonical.dbusmenu", "EventGroup"))
        {
            // 输入：a(isvu) —— 事件数组 (id, eventId, variant data, uint32 timestamp)
            DBusMessageIter args;
            dbus_message_iter_init(msg, &args);

            if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY)
            {
                DBusMessageIter arr;
                dbus_message_iter_recurse(&args, &arr);
                while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT)
                {
                    DBusMessageIter str;
                    dbus_message_iter_recurse(&arr, &str);

                    int id = 0;
                    const char *event_id = nullptr;

                    if (dbus_message_iter_get_arg_type(&str) == DBUS_TYPE_INT32)
                    {
                        dbus_message_iter_get_basic(&str, &id);
                    }
                    dbus_message_iter_next(&str);

                    if (dbus_message_iter_get_arg_type(&str) == DBUS_TYPE_STRING)
                    {
                        dbus_message_iter_get_basic(&str, &event_id);
                    }

                    std::fprintf(stderr, "[waty-tray] dbusmenu EventGroup: id=%d event_id=%s\n",
                                 id, event_id ? event_id : "(null)");
                    std::fflush(stderr);

                    if (event_id && std::strcmp(event_id, "clicked") == 0)
                    {
                        const auto it = flat_.find(id);
                        if (it != flat_.end() && it->second.item && it->second.item->on_click)
                        {
                            std::fprintf(stderr, "[waty-tray] EventGroup 触发菜单项 id=%d label=%s\n",
                                         id, it->second.item->label.c_str());
                            std::fflush(stderr);
                            const_cast<MenuItem *>(it->second.item)->on_click();
                        }
                    }

                    dbus_message_iter_next(&arr);
                }
            }

            // 输出：ai idErrors（空数组）
            DBusMessage *reply = dbus_message_new_method_return(msg);
            DBusMessageIter out_iter, out_arr;
            dbus_message_iter_init_append(reply, &out_iter);
            dbus_message_iter_open_container(&out_iter, DBUS_TYPE_ARRAY, "i", &out_arr);
            dbus_message_iter_close_container(&out_iter, &out_arr);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_is_method_call(msg, "com.canonical.dbusmenu", "AboutToShow"))
        {
            DBusMessage *reply = dbus_message_new_method_return(msg);
            dbus_bool_t need_update = FALSE;
            dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &need_update, DBUS_TYPE_INVALID);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_is_method_call(msg, "com.canonical.dbusmenu", "AboutToShowGroup"))
        {
            // 输入：ai ids；输出：ai updatesNeeded（空）+ ai idErrors（空）
            DBusMessage *reply = dbus_message_new_method_return(msg);
            DBusMessageIter out_iter, arr1, arr2;
            dbus_message_iter_init_append(reply, &out_iter);
            dbus_message_iter_open_container(&out_iter, DBUS_TYPE_ARRAY, "i", &arr1);
            dbus_message_iter_close_container(&out_iter, &arr1);
            dbus_message_iter_open_container(&out_iter, DBUS_TYPE_ARRAY, "i", &arr2);
            dbus_message_iter_close_container(&out_iter, &arr2);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_is_method_call(msg, "com.canonical.dbusmenu", "GetGroupProperties"))
        {
            // 参数：ai ids, as propertyNames
            DBusMessageIter args;
            dbus_message_iter_init(msg, &args);
            std::vector<int> ids;
            if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY)
            {
                DBusMessageIter arr;
                dbus_message_iter_recurse(&args, &arr);
                while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_INT32)
                {
                    int id = 0;
                    dbus_message_iter_get_basic(&arr, &id);
                    ids.push_back(id);
                    dbus_message_iter_next(&arr);
                }
            }

            DBusMessage *reply = dbus_message_new_method_return(msg);
            DBusMessageIter out_iter, arr;
            dbus_message_iter_init_append(reply, &out_iter);
            dbus_message_iter_open_container(&out_iter, DBUS_TYPE_ARRAY, "(ia{sv})", &arr);

            for (const int id : ids)
            {
                const auto it = flat_.find(id);
                if (it == flat_.end())
                {
                    continue;
                }

                DBusMessageIter item_struct, props;
                dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &item_struct);
                dbus_message_iter_append_basic(&item_struct, DBUS_TYPE_INT32, &id);
                dbus_message_iter_open_container(&item_struct, DBUS_TYPE_ARRAY, "{sv}", &props);
                append_menu_property(&props, it->second, "label");
                append_menu_property(&props, it->second, "enabled");
                append_menu_property(&props, it->second, "visible");
                append_menu_property(&props, it->second, "type");
                if (it->second.item->type == MenuItemType::Checkbox)
                {
                    append_menu_property(&props, it->second, "toggle-type");
                    append_menu_property(&props, it->second, "toggle-state");
                }
                if (it->second.item->type == MenuItemType::Submenu)
                {
                    append_menu_property(&props, it->second, "children-display");
                }
                dbus_message_iter_close_container(&item_struct, &props);
                dbus_message_iter_close_container(&arr, &item_struct);
            }

            dbus_message_iter_close_container(&out_iter, &arr);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (dbus_message_is_method_call(msg, "com.canonical.dbusmenu", "GetProperty"))
        {
            int id = 0;
            const char *name = nullptr;
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_INT32, &id,
                                       DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID))
            {
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }

            const auto it = flat_.find(id);
            const bool found = (it != flat_.end() && it->second.item);

            DBusMessage *reply = dbus_message_new_method_return(msg);
            DBusMessageIter out_iter, variant;
            dbus_message_iter_init_append(reply, &out_iter);
            dbus_message_iter_open_container(&out_iter, DBUS_TYPE_VARIANT, "s", &variant);

            const char *value = "";
            if (found && name)
            {
                if (std::strcmp(name, "label") == 0) value = it->second.item->label.c_str();
                else if (std::strcmp(name, "type") == 0)
                    value = (it->second.item->type == MenuItemType::Separator) ? "separator" : "standard";
                else if (std::strcmp(name, "children-display") == 0) value = "submenu";
            }
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);

            dbus_message_iter_close_container(&out_iter, &variant);
            dbus_connection_send(conn_, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
} // namespace waty::tray::detail
