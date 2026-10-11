#pragma once

// whiz 内部共享头文件（仅 src/ 内部使用，不对外暴露）。

#include <whiz/application.hpp>
#include <whiz/detail/embedded.hpp>
#include <whiz/web_window.hpp>

#include <saucer/app.hpp>
#include <saucer/icon.hpp>
#include <saucer/scheme.hpp>
#include <saucer/webview.hpp>
#include <saucer/window.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace whiz::detail
{
    /// 事件发射器：回调签名 void(Event&, const nlohmann::json&)。
    /// 回调在 UI 线程执行；unsubscribe 线程安全；emit 返回是否有回调调用了 preventDefault。
    class EventEmitter
    {
        struct Entry
        {
            std::function<void(Event &, const nlohmann::json &)> callback;
            std::mutex mutex;
            bool alive = true;
        };

      public:
        Subscription on(std::function<void(Event &, const nlohmann::json &)> callback)
        {
            auto entry = std::make_shared<Entry>();
            entry->callback = std::move(callback);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_entries.push_back(entry);
            }

            return Subscription([entry] {
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->alive = false;
            });
        }

        bool emit(const nlohmann::json &data = nlohmann::json::object())
        {
            std::vector<std::shared_ptr<Entry>> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                snapshot = m_entries;
            }

            Event event;
            for (const auto &entry : snapshot)
            {
                bool alive = true;
                {
                    std::lock_guard<std::mutex> lock(entry->mutex);
                    alive = entry->alive;
                }
                if (!alive)
                {
                    continue;
                }

                try
                {
                    entry->callback(event, data);
                }
                catch (...)
                {
                    // TODO(doc-gap): 记录日志，不中断其它监听器
                }
            }

            return event.defaultPrevented();
        }

      private:
        mutable std::mutex m_mutex;
        std::vector<std::shared_ptr<Entry>> m_entries;
    };

    /// 按事件名分组的发射器注册表。
    class EventRegistry
    {
      public:
        Subscription on(std::string event, std::function<void(Event &, const nlohmann::json &)> callback)
        {
            return emitter_for(std::move(event))->on(std::move(callback));
        }

        bool emit(std::string_view event, const nlohmann::json &data = nlohmann::json::object())
        {
            auto emitter = find(std::string(event));
            if (!emitter)
            {
                return false;
            }
            return emitter->emit(data);
        }

      private:
        std::shared_ptr<EventEmitter> emitter_for(std::string event)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto &entry = m_emitters[event];
            if (!entry)
            {
                entry = std::make_shared<EventEmitter>();
            }
            return entry;
        }

        std::shared_ptr<EventEmitter> find(const std::string &event)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_emitters.find(event);
            if (it == m_emitters.end())
            {
                return nullptr;
            }
            return it->second;
        }

        std::mutex m_mutex;
        std::unordered_map<std::string, std::shared_ptr<EventEmitter>> m_emitters;
    };

    /// 投递回调到 UI 线程执行（跨线程安全）。
    void post_to_ui_thread(saucer::application *app, std::function<void()> callback);

    /// 应用日志（控制台输出）。TODO(doc-gap): 阶段 2 仅控制台，logFile 写入后续补齐。
    void log_message(std::string_view level, std::string_view message);
} // namespace whiz::detail

namespace whiz
{
    /// WebWindow 内部实现（在 src/detail.hpp 中统一定义，供多个源文件共享）。
    struct WebWindow::Impl
    {
        Application *app           = nullptr;
        saucer::application *saucer_app = nullptr;
        WindowId id                = 0;
        PermissionOptions permissions;

        // 选项拷贝（string_view 生命周期由内部保证）
        std::string title;
        int width      = 800;
        int height     = 600;
        int min_width  = 0;
        int min_height = 0;
        bool resizable  = true;
        bool fullscreen = false;
        bool frame      = true;
        std::weak_ptr<WebWindow> parent;
        std::string icon;

        // 原生对象
        std::shared_ptr<saucer::window> window;
        std::unique_ptr<saucer::webview> view;

        std::mutex mutex;
        bool realized = false;
        bool closed   = false;

        // 窗口状态缓存（任意线程可读）
        std::pair<int, int> cached_size{0, 0};
        std::pair<int, int> cached_position{0, 0};
        bool cached_visible   = false;
        bool cached_minimized = false;
        bool cached_maximized = false;

        // 排队中的操作（run() 前调用）
        std::optional<std::string> pending_entry;
        std::optional<std::string> pending_url;
        bool pending_show = false;

        // 事件
        detail::EventRegistry window_events;  // windowEventHandle 回调（按事件名分组）

        // IPC 处理器条目
        struct IpcHandlerEntry
        {
            std::function<std::future<nlohmann::json>(const nlohmann::json &)> callback;
            std::mutex mutex;
            bool alive = true;
        };

        std::mutex ipc_mutex;
        std::unordered_map<std::string, std::vector<std::shared_ptr<IpcHandlerEntry>>> ipc_handlers;

        // saucer 原生事件监听 id（用于 off）
        std::unordered_map<std::string, std::size_t> saucer_listeners;

        // JS 桥是否已注入
        bool bridge_ready = false;
    };

    /// Application 内部实现。
    struct Application::Impl
    {
        std::unique_ptr<saucer::application> app;

        std::vector<std::shared_ptr<WebWindow>> windows;
        mutable std::mutex windows_mutex;
        std::uint64_t next_id = 1;

        std::string app_name;
        std::string icon;
        std::optional<std::string> log_file;
        PermissionOptions permissions;

        // 应用生命周期事件
        detail::EventRegistry events;

        // onEventLoopTick 任务链
        struct TickTask
        {
            std::function<void()> callback;
            std::atomic<bool> alive{true};
        };
        std::mutex tick_mutex;
        std::vector<std::shared_ptr<TickTask>> tick_tasks;
        bool tick_chain_running = false;
        bool tick_iterating     = false;

        // 单实例锁
        bool single_instance = false;
        int lock_fd          = -1;
        int instance_socket  = -1;
        std::string sock_path;
        std::jthread instance_thread;

        // 已解析的应用图标（setIcon / ApplicationOptions.icon）
        std::optional<saucer::icon> resolved_icon;

        // 已打开窗口计数（用于 window-all-closed）
        std::atomic<std::size_t> open_windows{0};
    };
} // namespace whiz

namespace whiz::detail
{
    // 以下函数在其它源文件中实现：

    // js_bridge.cpp：注入 JS 桥、处理来自 JS 的消息
    void setup_js_bridge(WebWindow::Impl &impl);
    void handle_js_message(WebWindow::Impl &impl, std::string_view message);

    // js_bridge.cpp：响应/推送（供 fs / dialog 桥复用）
    void respond(WebWindow::Impl &impl, std::uint64_t id, const nlohmann::json &result);
    void respond_error(WebWindow::Impl &impl, std::uint64_t id, const nlohmann::json &error);
    void emit_to_js(WebWindow::Impl &impl, std::string_view event, const nlohmann::json &data);

    // fs_bridge.cpp / dialog_bridge.cpp 入口
    void handle_fs_request(WebWindow::Impl &impl, const nlohmann::json &request);
    void handle_dialog_request(WebWindow::Impl &impl, const nlohmann::json &request);

    // web_window.cpp：图标路径解析（先查嵌入资源，再查可执行文件目录）
    std::string executable_dir();
    std::optional<saucer::icon> resolve_icon(std::string_view path);

    // web_window.cpp：应用窗口图标（GTK 后端通过 gdk_toplevel_set_icon_list 设置）
    void apply_window_icon(saucer::window &window, const saucer::icon &icon);
}
