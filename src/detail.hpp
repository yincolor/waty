#pragma once

// waty 内部共享头文件（仅 src/ 内部使用，不对外暴露）。
//
// 这里集中定义了三类内容：
//   1. 跨源文件复用的事件基础设施（EventEmitter / EventRegistry）；
//   2. WebWindow::Impl 与 Application::Impl 的完整定义（PImpl 的实现侧）；
//   3. src/ 内部函数声明（JS 桥、fs/dialog/sqlite3/notify 桥、日志、图标）。
//
// 之所以把 Impl 放在共享头文件而不是各自的 .cpp：
//   application.cpp / web_window.cpp / js_bridge.cpp / *_bridge.cpp 需要互相访问对方的
//   内部状态（如注入 JS 桥、响应消息、注销 SQLite 句柄），拆开会导致大量转发接口。
//
// 线程约定：
//   * window->m_impl->mutex 保护窗口配置与原生句柄；
//   * m_impl->ipc_mutex 保护 IPC handler 表；
//   * EventRegistry 内部自带互斥锁，事件名到发射器的映射可并发访问。

#include <waty/application.hpp>
#include <waty/detail/embedded.hpp>
#include <waty/web_window.hpp>

#include "path_internal.hpp"

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

namespace waty::detail
{
    class Sqlite3Service; // 前置声明（定义在 src/sqlite3_service.hpp，仅 src/ 内部使用）

    /// 事件发射器：回调签名 void(Event&, const nlohmann::json&)。
    /// 回调在 UI 线程执行；unsubscribe 线程安全；emit 返回是否有回调调用了 preventDefault。
    class EventEmitter
    {
        // 每个监听器独立持有 alive 标志与互斥锁：unsubscribe 只需翻转标志，
        // 不会阻塞正在执行的回调，也不会使快照中的 shared_ptr 失效。
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

        /// 同步调用所有仍存活的监听器。
        /// 先对列表做快照再逐个调用，避免回调中注册/取消导致迭代器失效；
        /// 单个回调抛异常会被吞掉，不影响后续监听器。
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
    /// 对应 Public API 的 Application::on / WebWindow::windowEventHandle：
    /// 同一事件名可以有多个监听器；事件名不存在时 emit 返回 false。
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
} // namespace waty::detail

namespace waty
{
    /// WebWindow 内部实现（在 src/detail.hpp 中统一定义，供多个源文件共享）。
    struct WebWindow::Impl
    {
        Application *app           = nullptr;  // 所属应用（生命周期长于窗口）
        saucer::application *saucer_app = nullptr;  // 底层运行时，realize() 后有效
        WindowId id                = 0;
        PermissionOptions permissions;   // 构造时从 Application 拷贝（应用级权限）
        std::string app_name;            // 供路径模块拼接应用目录
        std::string organization_name;

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

        // WebView 运行时数据目录（已解析；为空表示回退 Saucer 默认）
        std::optional<std::string> storage_path;

        // 原生对象（realize() 后创建；在此之前为 null）
        std::shared_ptr<saucer::window> window;
        std::unique_ptr<saucer::webview> view;

        // 保护整个 Impl 的可变字段；递归调用要小心（事件回调可能重入）
        std::mutex mutex;
        bool realized = false;  // 原生窗口是否已创建
        bool closed   = false;  // 窗口是否已关闭（closed 事件后置位）

        // 窗口状态缓存（任意线程可读）。
        // 由构造参数初始化，并由 resize / move / maximize / minimize 等原生事件持续更新；
        // isXxx() / getSize() / getPosition() 直接读缓存，避免跨线程访问原生句柄。
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
        // 单 handler 模型：每个事件名至多一个 handler（见 20261014 修复文档 1.2）。
        std::unordered_map<std::string, std::shared_ptr<IpcHandlerEntry>> ipc_handlers;

        // saucer 原生事件监听 id（用于 off；当前注册后不注销，保留以兼容后续扩展）
        std::unordered_map<std::string, std::size_t> saucer_listeners;

        // JS 桥是否已注入（避免 realize 重入时重复注入脚本/监听器）
        bool bridge_ready = false;

        // JS 侧 cwd 缓存（每轮事件循环刷新；见 js_bridge.cpp）。
        // path.cwd() 是同步 JS API，不能每次调用都跨进程/线程查询，因此 C++ 侧
        // 主动轮询并把结果推给页面；页面首次拿到的是 realize 时的快照。
        std::string js_cwd;
        std::string js_cwd_error;
        Subscription cwd_subscription;

        // Sqlite3 服务（由 Application 下发，窗口与 App 共享同一实例）
        std::shared_ptr<detail::Sqlite3Service> sqlite3;
    };

    /// Application 内部实现。
    struct Application::Impl
    {
        // 底层 Saucer 应用：构造时创建，run() 期间驱动 UI 事件循环。
        std::unique_ptr<saucer::application> app;

        // 窗口集合。windows_mutex 保护该 vector 与 next_id；
        // windows 中保留 shared_ptr，窗口关闭后由用户/内部引用决定何时真正析构。
        std::vector<std::shared_ptr<WebWindow>> windows;
        mutable std::mutex windows_mutex;
        std::uint64_t next_id = 1;

        std::string app_name;
        std::string organization_name;
        std::string icon;
        std::string app_data_dir;
        std::string config_dir;
        std::optional<std::string> webview_storage_path;
        PermissionOptions permissions;

        // 应用生命周期事件
        detail::EventRegistry events;

        // onEventLoopTick 任务链。
        // 任务不在固定间隔的定时器上执行，而是利用 Saucer 的 post() 在每轮 UI 事件循环
        // 末尾重新投递自己（见 Application::run_tick_loop），因此开销与帧率相关。
        struct TickTask
        {
            std::function<void()> callback;
            std::atomic<bool> alive{true};  // unsubscribe 置 false，下轮清理
        };
        std::mutex tick_mutex;
        std::vector<std::shared_ptr<TickTask>> tick_tasks;
        bool tick_chain_running = false;  // 任务链是否仍在自续投递
        bool tick_iterating     = false;  // 是否正在执行回调（防止回调内注册）

        // 单实例锁（Linux：flock 锁文件 + Unix socket 监听；其它平台暂未实现）。
        // 第二个实例启动时连接 socket，把 argv/cwd 发给主实例。
        bool single_instance = false;
        int lock_fd          = -1;  // 锁文件 fd（flock）
        int instance_socket  = -1;  // 监听 socket fd
        std::string sock_path;
        std::jthread instance_thread;

        // 已解析的应用图标（setIcon / ApplicationOptions.icon）
        std::optional<saucer::icon> resolved_icon;

        // 已打开窗口计数（用于 window-all-closed）
        std::atomic<std::size_t> open_windows{0};

        // Sqlite3 服务（构造时创建；只保存路径与上限，不建目录/不起线程）
        std::shared_ptr<detail::Sqlite3Service> sqlite3;
    };
} // namespace waty

namespace waty::detail
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
    void handle_sqlite3_request(WebWindow::Impl &impl, const nlohmann::json &request);
    void handle_notify_request(WebWindow::Impl &impl, const nlohmann::json &request);

    // application.cpp：全局日志 sink（线程安全）
    void set_log_file(std::optional<std::string_view> path);

    // web_window.cpp：图标路径解析（先查嵌入资源，再查可执行文件目录）
    std::optional<saucer::icon> resolve_icon(std::string_view path);

    // web_window.cpp：应用窗口图标（GTK 后端通过 gdk_toplevel_set_icon_list 设置）
    void apply_window_icon(saucer::window &window, const saucer::icon &icon);
}
