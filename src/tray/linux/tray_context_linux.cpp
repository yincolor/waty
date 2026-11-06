// Linux 托盘上下文：进程级共享 D-Bus 连接 + GLib 主循环集成。
//
// 设计：
//   * 所有托盘图标共享同一条会话总线连接（g_connection）；
//   * 把 libdbus 的 watch 映射为 GSource（g_unix_fd_source_new），
//     因此 D-Bus 消息在 GTK/GLib 主循环（UI 线程）上派发；
//   * 必须安装 watch 函数，否则 send_with_reply_and_block 阻塞读期间会丢弃
//     面板发来的其它消息（例如注册时立即到达的 GetAll）。
//   * platform_cleanup() 由 TrayContext 析构触发，幂等。

#include "../platform.hpp"

#include "sni_dbus.hpp"

#include <dbus/dbus.h>
#include <glib-unix.h>

#include <cstdio>
#include <mutex>

namespace waty::tray::detail
{
    namespace
    {
        DBusConnection *g_connection = nullptr;
        std::mutex g_mutex;

        // libdbus watch 集成到 GLib：只监听 G_IO_IN（可读）。
        // 写操作在回调末尾用 dbus_connection_flush 完成（托盘消息很小，不会阻塞）。
        // 注意：必须设置 watch 函数，否则 send_with_reply_and_block 在阻塞读取时会丢弃
        // 注册期间到达的其它消息（如面板立即发来的 GetAll）。
        struct WatchData
        {
            DBusConnection *conn = nullptr;
            DBusWatch *watch     = nullptr;
            GSource *source      = nullptr;
        };

        gboolean on_watch_ready(gint fd, GIOCondition condition, gpointer data)
        {
            (void)fd;
            auto *wd = static_cast<WatchData *>(data);

            if (condition & G_IO_IN)
            {
                dbus_watch_handle(wd->watch, DBUS_WATCH_READABLE);
                while (dbus_connection_dispatch(wd->conn) == DBUS_DISPATCH_DATA_REMAINS)
                {
                }
                // 把 dispatch 阶段入队的回复立即写出
                dbus_connection_flush(wd->conn);
            }
            return G_SOURCE_CONTINUE;
        }

        dbus_bool_t add_watch(DBusWatch *watch, void *data)
        {
            auto *conn = static_cast<DBusConnection *>(data);

            const int fd = dbus_watch_get_unix_fd(watch);
            if (fd < 0)
            {
                return FALSE;
            }

            auto *wd = new WatchData{conn, watch, nullptr};
            wd->source = g_unix_fd_source_new(fd, G_IO_IN);
            g_source_set_callback(wd->source, reinterpret_cast<GSourceFunc>(on_watch_ready), wd, nullptr);
            g_source_attach(wd->source, g_main_context_default());
            dbus_watch_set_data(watch, wd, nullptr);
            return TRUE;
        }

        void remove_watch(DBusWatch *watch, void *)
        {
            auto *wd = static_cast<WatchData *>(dbus_watch_get_data(watch));
            if (!wd)
            {
                return;
            }
            dbus_watch_set_data(watch, nullptr, nullptr);
            if (wd->source)
            {
                g_source_destroy(wd->source);
                g_source_unref(wd->source);
                wd->source = nullptr;
            }
            delete wd;
        }

        void toggled_watch(DBusWatch *watch, void *data)
        {
            // 只关心可读事件，flags 变化无需处理
            (void)watch;
            (void)data;
        }
    } // namespace

    DBusConnection *shared_connection()
    {
        return g_connection;
    }

    TrayResult<void> platform_initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_connection)
        {
            return {};
        }

        DBusError error;
        dbus_error_init(&error);

        DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &error);
        if (dbus_error_is_set(&error) || !conn)
        {
            dbus_error_free(&error);
            return std::unexpected(TrayError::DbusConnectionFailed);
        }
        dbus_error_free(&error);

        // 必须设置 watch 函数（见上方注释），让 send_with_reply_and_block 正确排队消息。
        dbus_connection_set_watch_functions(conn, add_watch, remove_watch, toggled_watch, conn, nullptr);

        g_connection = conn;
        return {};
    }

    void platform_cleanup()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_connection)
        {
            dbus_connection_set_watch_functions(g_connection, nullptr, nullptr, nullptr, nullptr, nullptr);
            dbus_connection_unref(g_connection);
            g_connection = nullptr;
        }
    }
} // namespace waty::tray::detail
