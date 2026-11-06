// 应用生命周期实现（见 DESIGN.md §7.3）。
//
// 本文件负责：
//   * 全局日志 sink（stderr + 可选文件，线程安全）；
//   * Application 的构造/析构、窗口注册表、应用事件白名单；
//   * onEventLoopTick 任务链（借助 Saucer::post 在每轮 UI 循环自续投递）；
//   * run() 的 UI 线程入口：realize 窗口、触发 ready / quit；
//   * Linux 单实例锁（flock + Unix domain socket）。
//
// 线程模型：
//   * UI 线程（Saucer/GTK 主线程）执行 run()、窗口 realize 与所有事件回调；
//   * 单实例监听使用独立 jthread，收到第二个实例的消息后 post 回 UI 线程；
//   * detail::post_to_ui_thread 是跨线程回投的唯一入口。

#include <waty/application.hpp>

#include "detail.hpp"

#include "sqlite3_service.hpp"

#include <saucer/app.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
    #include <fcntl.h>
    #include <sys/file.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

namespace waty::detail
{
    namespace
    {
        // 日志 sink 全局单例：mutex 同时保护 stderr 写入与文件流替换，
        // 保证多线程日志不会交错、set_log_file 不会与写入竞争。
        std::mutex g_log_mutex;
        std::unique_ptr<std::ofstream> g_log_stream;
    } // namespace

    // 把回调投递到 UI 线程。app 为空（例如窗口尚未 realize）时直接丢弃，
    // 调用方因此无需在每次跨线程回投时判空。
    void post_to_ui_thread(saucer::application *app, std::function<void()> callback)
    {
        if (!app)
        {
            return;
        }
        app->post(std::move(callback));
    }

    // 切换文件日志。语义：
    //   * nullopt        → 关闭并释放文件句柄；
    //   * 空字符串       → 视为关闭，但记录一条 warning；
    //   * 相对路径       → 基于可执行文件目录解析；
    //   * 目录不存在     → 自动创建；打开失败则保持关闭。
    // 先构造新流再在锁内替换，失败时旧流保持不变（但本实现失败会关闭日志）。
    void set_log_file(std::optional<std::string_view> path)
    {
        std::unique_ptr<std::ofstream> next;
        std::string warning;

        if (path && path->empty())
        {
            warning = "waty: logFile 为空字符串，已关闭文件日志";
        }
        else if (path)
        {
            try
            {
                std::filesystem::path target(*path);
                if (target.is_relative())
                {
                    target = std::filesystem::path(executable_dir()) / target;
                }

                if (!target.parent_path().empty())
                {
                    std::error_code ec;
                    std::filesystem::create_directories(target.parent_path(), ec);
                    if (ec)
                    {
                        warning = "waty: 无法创建日志目录 " + target.parent_path().string() +
                                  " (" + ec.message() + ")，已关闭文件日志";
                    }
                }

                if (warning.empty())
                {
                    next = std::make_unique<std::ofstream>(target, std::ios::app);
                    if (!*next)
                    {
                        warning = "waty: 无法打开日志文件 " + target.string() + "，已关闭文件日志";
                        next.reset();
                    }
                }
            }
            catch (const std::exception &ex)
            {
                warning = "waty: 日志文件路径非法: " + std::string(ex.what());
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            g_log_stream = std::move(next);
        }

        if (!warning.empty())
        {
            log_message("warn", warning);
        }
    }

    // 写一条日志：[waty][level] message。
    // 始终输出到 stderr，并在启用文件日志时追加到文件；每次写完都 flush，
    // 便于崩溃后保留最后几条日志（性能要求不高的桌面场景可接受）。
    void log_message(std::string_view level, std::string_view message)
    {
        std::string line;
        line.reserve(level.size() + message.size() + 16);
        line += "[waty][";
        line += level;
        line += "] ";
        line += message;
        line += '\n';

        std::lock_guard<std::mutex> lock(g_log_mutex);
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);

        if (g_log_stream && *g_log_stream)
        {
            g_log_stream->write(line.data(), static_cast<std::streamsize>(line.size()));
            g_log_stream->flush();
        }
    }
} // namespace waty::detail

namespace waty
{
    namespace
    {
        // 应用事件白名单：Application::on() 只接受这些事件名，
        // 拼写错误会立即抛 std::invalid_argument，而不是静默失效。
        const std::unordered_set<std::string> k_app_events = {
            "ready",
            "before-quit",
            "will-quit",
            "quit",
            "activate",
            "window-all-closed",
            "second-instance",
            "js-error-crash",
        };

        // 读取当前进程命令行，用于 second-instance 事件负载。
        // Linux 通过 /proc/self/cmdline（NUL 分隔）；其它平台暂未实现，返回空数组。
        std::vector<std::string> current_argv()
        {
            std::vector<std::string> argv;
#if defined(__linux__)
            std::ifstream file("/proc/self/cmdline", std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(file)), {});
            std::size_t start = 0;
            while (start <= data.size())
            {
                const auto end = data.find('\0', start);
                const auto token = data.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (token.empty())
                {
                    break;
                }
                argv.push_back(token);
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }
#else
            // TODO(platform): Windows/macOS 命令行参数
#endif
            return argv;
        }

    } // namespace

    // 构造阶段按顺序完成：
    //   1. 校验 appName / organizationName（会拼进用户目录，必须防路径穿越）；
    //   2. 保存配置、初始化日志、创建数据目录路径与 Sqlite3Service；
    //   3. 创建 saucer::application；
    //   4. 预解析图标、挂接 quit 事件、注册 embedded:// 协议。
    // 第 2 步只计算路径不建目录；数据库文件在首次 open 时才落地。
    Application::Application(const ApplicationOptions &options)
        : m_impl(std::make_unique<Impl>())
    {
        if (!detail::is_valid_directory_name(options.appName))
        {
            detail::log_message("error", "waty: ApplicationOptions.appName 非法: '" + options.appName + "'");
            throw std::invalid_argument(
                "waty: ApplicationOptions.appName 必须非空、匹配 ^[A-Za-z0-9_.]+$、长度不超过 64，且不得以 . 开头或结尾，"
                "且不得为 Windows 保留设备名（CON/PRN/AUX/NUL/COM1-9/LPT1-9）。"
                "实际值: " + options.appName);
        }
        if (!detail::is_valid_directory_name(options.organizationName))
        {
            detail::log_message("error", "waty: ApplicationOptions.organizationName 非法: '" + options.organizationName + "'");
            throw std::invalid_argument(
                "waty: ApplicationOptions.organizationName 必须非空、匹配 ^[A-Za-z0-9_.]+$、长度不超过 64，且不得以 . 开头或结尾，"
                "且不得为 Windows 保留设备名（CON/PRN/AUX/NUL/COM1-9/LPT1-9）。"
                "实际值: " + options.organizationName);
        }

        m_impl->app_name          = options.appName;
        m_impl->organization_name = options.organizationName;
        m_impl->icon              = options.icon;
        m_impl->permissions       = options.permissions;
        if (options.webviewStoragePath && !options.webviewStoragePath->empty())
        {
            m_impl->webview_storage_path = *options.webviewStoragePath;
        }

        // 文件日志：std::nullopt 表示关闭；空字符串由 set_log_file 记录 warning 后关闭。
        detail::set_log_file(options.logFile
                                 ? std::optional<std::string_view>(*options.logFile)
                                 : std::nullopt);

        // Sqlite3 上限校验（见设计文档 §4.5 / §7.1）
        if (options.sqlite3.maxOpenDatabases < 1 || options.sqlite3.maxOpenDatabases > 1024)
        {
            throw std::invalid_argument("waty: ApplicationOptions.sqlite3.maxOpenDatabases 必须在 1..1024 之间");
        }

        m_impl->app_data_dir = path::getPath(SysPath::AppData, options);
        m_impl->config_dir   = path::getPath(SysPath::Config, options);

        // 只保存数据目录与上限：目录与 .db 在首次 open 时才落地。
        m_impl->sqlite3 = std::make_shared<detail::Sqlite3Service>(m_impl->app_data_dir,
                                                                  options.sqlite3.maxOpenDatabases);

        const std::string id = m_impl->app_name;

        // quit_on_last_window_closed 设为 false：由 waty 自行处理 window-all-closed 默认行为，
        // 以保证 before-quit / will-quit / quit 生命周期事件完整触发。
        auto created = saucer::application::create({
            .id                       = id,
            .quit_on_last_window_closed = false,
        });
        if (!created)
        {
            throw std::runtime_error("waty: 创建 saucer::application 失败: " + created.error().message());
        }
        m_impl->app = std::make_unique<saucer::application>(std::move(*created));

        // 预解析应用图标（先查嵌入资源，再查可执行文件目录）
        if (!m_impl->icon.empty())
        {
            m_impl->resolved_icon = detail::resolve_icon(m_impl->icon);
            if (!m_impl->resolved_icon)
            {
                detail::log_message("warn", "waty: ApplicationOptions.icon 未找到，使用系统默认图标: " + m_impl->icon);
            }
        }

        // 挂接 saucer 的 quit 事件：映射为 waty 的 before-quit / will-quit。
        m_impl->app->on<saucer::application::event::quit>({{.func = [this]() -> saucer::policy {
            if (m_impl->events.emit("before-quit"))
            {
                return saucer::policy::block;
            }
            if (m_impl->events.emit("will-quit"))
            {
                return saucer::policy::block;
            }
            return saucer::policy::allow;
        }, .clearable = false}});

        // 注册 embedded:// 协议（幂等）。
        saucer::webview::register_scheme("embedded");
    }

    Application::~Application()
    {
        releaseSingleInstanceLock();
        detail::set_log_file(std::nullopt);
    }

    void Application::setLogFile(std::optional<std::string_view> path)
    {
        detail::set_log_file(path);
    }

    // 更新应用图标并同步到所有已 realize 的窗口。
    // 注意：这里按“窗口当前图标路径”判断会丢失“继承应用图标”的信息，
    // 因此实际行为是所有窗口都改用新应用图标（窗口级 icon 已被覆盖为应用图标）。
    void Application::setIcon(std::string_view iconPath)
    {
        m_impl->icon          = std::string(iconPath);
        m_impl->resolved_icon = detail::resolve_icon(m_impl->icon);
        if (!m_impl->resolved_icon)
        {
            detail::log_message("warn", "waty: setIcon 未找到图标，使用系统默认图标: " + m_impl->icon);
        }

        std::lock_guard<std::mutex> lock(m_impl->windows_mutex);
        for (const auto &window : m_impl->windows)
        {
            std::lock_guard<std::mutex> wlock(window->m_impl->mutex);
            if (window->m_impl->window && m_impl->resolved_icon)
            {
                detail::apply_window_icon(*window->m_impl->window, *m_impl->resolved_icon);
            }
        }
    }

    std::shared_ptr<WebWindow> Application::createWindow(const WebWindowOptions &options)
    {
        // 尺寸校验（文档 6.4）：负值抛 std::invalid_argument
        if (options.width < 0 || options.height < 0 || options.minWidth < 0 || options.minHeight < 0)
        {
            throw std::invalid_argument("waty: 窗口宽度/高度/最小宽度/最小高度不能为负值");
        }
        // TODO(phase2): 宽度/高度/最小尺寸超过屏幕分辨率 → std::out_of_range

        const WindowId id = m_impl->next_id++;
        std::shared_ptr<WebWindow> window(new WebWindow(this, id, options));
        window->m_impl->sqlite3 = m_impl->sqlite3; // shared_ptr 拷贝，窗口与 App 共享同一 Service

        {
            std::lock_guard<std::mutex> lock(m_impl->windows_mutex);
            m_impl->windows.push_back(window);
        }
        m_impl->open_windows.fetch_add(1);

        return window;
    }

    std::optional<std::shared_ptr<WebWindow>> Application::getWindow(WindowId id) const
    {
        std::lock_guard<std::mutex> lock(m_impl->windows_mutex);
        for (const auto &window : m_impl->windows)
        {
            if (window->id() == id)
            {
                return window;
            }
        }
        return std::nullopt;
    }

    std::vector<std::shared_ptr<WebWindow>> Application::windows() const
    {
        std::lock_guard<std::mutex> lock(m_impl->windows_mutex);
        return m_impl->windows;
    }

    Subscription Application::on(std::string_view event,
                                 std::function<void(Event &, const nlohmann::json &)> callback)
    {
        if (!k_app_events.contains(std::string(event)))
        {
            throw std::invalid_argument("waty: 未知应用事件: " + std::string(event));
        }
        return m_impl->events.on(std::string(event), std::move(callback));
    }

    // 注册每轮事件循环任务。
    // 任务被 shared_ptr 持有；返回的 Subscription 只把 alive 置 false，
    // 真正的清理发生在 run_tick_loop 末尾（erase_if），因此回调执行期间列表稳定。
    Subscription Application::onEventLoopTick(std::function<void()> callback)
    {
        auto task = std::make_shared<Impl::TickTask>();
        task->callback = std::move(callback);

        {
            std::lock_guard<std::mutex> lock(m_impl->tick_mutex);
            if (m_impl->tick_iterating)
            {
                // 文档 7.3.3：回调中禁止注册/取消任务
                assert(false && "waty: onEventLoopTick 回调中禁止注册任务");
                std::abort();
            }
            m_impl->tick_tasks.push_back(task);
        }

        start_tick_chain();

        return Subscription([task] {
            task->alive.store(false);
        });
    }

    // 启动任务链（幂等）：如果已经在运行则直接返回，避免重复投递造成多链并行。
    void Application::start_tick_chain()
    {
        std::lock_guard<std::mutex> lock(m_impl->tick_mutex);
        if (m_impl->tick_chain_running)
        {
            return;
        }
        m_impl->tick_chain_running = true;
        detail::post_to_ui_thread(m_impl->app.get(), [this] { run_tick_loop(); });
    }

    // 一轮任务执行：快照存活回调 → 逐个执行 → 清理已取消任务 → 若仍有任务则 post 自续。
    // tick_iterating 用于阻止回调中注册/取消（文档 7.3.3），重入直接 abort。
    void Application::run_tick_loop()
    {
        if (m_impl->tick_iterating)
        {
            assert(false && "waty: onEventLoopTick 重入");
            std::abort();
        }
        m_impl->tick_iterating = true;

        std::vector<std::function<void()>> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_impl->tick_mutex);
            for (const auto &task : m_impl->tick_tasks)
            {
                if (task->alive.load())
                {
                    snapshot.push_back(task->callback);
                }
            }
        }

        for (const auto &cb : snapshot)
        {
            try
            {
                cb();
            }
            catch (...)
            {
                detail::log_message("warn", "onEventLoopTick 回调抛出异常");
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_impl->tick_mutex);
            std::erase_if(m_impl->tick_tasks, [](const auto &task) { return !task->alive.load(); });
            m_impl->tick_chain_running = !m_impl->tick_tasks.empty();
        }
        m_impl->tick_iterating = false;

        if (m_impl->tick_chain_running)
        {
            detail::post_to_ui_thread(m_impl->app.get(), [this] { run_tick_loop(); });
        }
    }

    void Application::quit()
    {
        // 交给 Saucer/平台事件循环处理；before-quit / will-quit 在 quit 事件中拦截。
        m_impl->app->quit();
    }

    // 运行 UI 事件循环。协程体内在 UI 线程执行：
    //   1. 为所有已创建窗口 realize 原生对象（期间可能抛异常，会让 run 直接失败）；
    //   2. 触发 ready；
    //   3. 等待事件循环结束；
    //   4. 触发 quit（此时已无法阻止退出）。
    int Application::run()
    {
        return m_impl->app->run([this](saucer::application *saucer_app) -> coco::stray {
            // UI 线程（GTK 主线程）：创建所有已排队窗口的原生实现
            for (auto &window : m_impl->windows)
            {
                window->realize(saucer_app);
            }

            // 触发 ready 事件
            m_impl->events.emit("ready");

            co_await saucer_app->finish();

            // 事件循环退出，触发 quit 事件（已退出）
            m_impl->events.emit("quit");
        });
    }

    std::optional<std::vector<std::uint8_t>> Application::readEmbeddedFile(std::string_view path) const
    {
        const auto content = detail::find_embedded_resource(path);
        if (!content)
        {
            return std::nullopt;
        }

        const auto *begin = reinterpret_cast<const unsigned char *>(content->data());
        return std::vector<std::uint8_t>(begin, begin + content->size());
    }

    // Linux 单实例协议：
    //   主实例：open(lock) → flock(LOCK_EX|LOCK_NB) 成功 → 监听 <app>.sock；
    //   次实例：flock 失败 → connect(<app>.sock) → 发送 {argv, cwd} → 返回 false。
    // 锁文件与 socket 都位于配置目录（Config），确保同一用户下互斥。
    bool Application::requestSingleInstanceLock()
    {
        if (m_impl->single_instance)
        {
            return true;
        }

#if defined(__linux__)
        const std::string user_data = m_impl->config_dir;
        std::error_code ec;
        std::filesystem::create_directories(user_data, ec);

        const std::string lock_file = user_data + "/" + m_impl->app_name + ".lock";
        const std::string sock_path = user_data + "/" + m_impl->app_name + ".sock";

        const int fd = ::open(lock_file.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd < 0)
        {
            detail::log_message("error", "requestSingleInstanceLock: 无法创建锁文件");
            return false;
        }

        if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            // 已有主实例：通知它
            ::close(fd);
            notify_primary(sock_path);
            return false;
        }

        m_impl->lock_fd        = fd;
        m_impl->sock_path      = sock_path;
        m_impl->single_instance = true;
        start_instance_listener();
        return true;
#else
        // TODO(platform): Windows/macOS 单实例锁
        m_impl->single_instance = true;
        return true;
#endif
    }

    void Application::releaseSingleInstanceLock()
    {
        if (!m_impl->single_instance)
        {
            return;
        }

#if defined(__linux__)
        if (m_impl->instance_socket >= 0)
        {
            ::shutdown(m_impl->instance_socket, SHUT_RDWR);
            ::close(m_impl->instance_socket);
            m_impl->instance_socket = -1;
        }
        if (m_impl->instance_thread.joinable())
        {
            m_impl->instance_thread.request_stop();
            m_impl->instance_thread.join();
        }
        if (m_impl->lock_fd >= 0)
        {
            ::flock(m_impl->lock_fd, LOCK_UN);
            ::close(m_impl->lock_fd);
            m_impl->lock_fd = -1;
        }
        if (!m_impl->sock_path.empty())
        {
            ::unlink(m_impl->sock_path.c_str());
        }
#endif

        m_impl->single_instance = false;
    }

    bool Application::hasSingleInstanceLock() const
    {
        return m_impl->single_instance;
    }

    void Application::emitInternalEvent(std::string_view event, const nlohmann::json &data)
    {
        m_impl->events.emit(event, data);
    }

    // 窗口 closed 回调：递减计数并在归零时触发 window-all-closed。
    // 默认行为：非 macOS 退出应用，macOS 保持运行；监听器可 preventDefault() 阻止退出。
    void Application::handleWindowClosed()
    {
        const auto remaining = m_impl->open_windows.fetch_sub(1) - 1;
        if (remaining > 0)
        {
            return;
        }

        // 所有窗口已关闭：触发 window-all-closed
        const bool prevented = m_impl->events.emit("window-all-closed");

#if !defined(__APPLE__)
        // 非 macOS 默认退出应用（文档 7.3.1）
        if (!prevented)
        {
            quit();
        }
#else
        (void)prevented;
        // macOS 默认不退出
#endif
    }

    // 在 UI 线程分发 second-instance。
    // 负载补充 windowId：优先取当前第一个窗口，没有窗口时为 0；
    // 调用方（主实例）可据此聚焦已有窗口。
    void Application::dispatchSecondInstance(nlohmann::json data)
    {
        data["windowId"] = [&] {
            std::lock_guard<std::mutex> lock(m_impl->windows_mutex);
            return m_impl->windows.empty() ? WindowId{0} : m_impl->windows.front()->id();
        }();
        m_impl->events.emit("second-instance", data);
    }

#if defined(__linux__)
    // 启动 Unix domain socket 监听线程。
    // 每次 accept 后读取全部字节，按 JSON 解析；解析失败/空消息按空对象处理，
    // 然后 post 到 UI 线程触发 second-instance（错误数据不会让监听线程退出）。
    void Application::start_instance_listener()
    {
        const std::string sock_path = m_impl->sock_path;

        m_impl->instance_thread = std::jthread([this, sock_path](std::stop_token stop) {
            const int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (sfd < 0)
            {
                return;
            }
            m_impl->instance_socket = sfd;

            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
            ::unlink(sock_path.c_str());

            if (::bind(sfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            {
                ::close(sfd);
                m_impl->instance_socket = -1;
                return;
            }
            if (::listen(sfd, 8) != 0)
            {
                ::close(sfd);
                m_impl->instance_socket = -1;
                return;
            }

            while (!stop.stop_requested())
            {
                const int cfd = ::accept(sfd, nullptr, nullptr);
                if (cfd < 0)
                {
                    break;
                }

                std::string data;
                char buffer[4096];
                ssize_t n;
                while ((n = ::read(cfd, buffer, sizeof(buffer))) > 0)
                {
                    data.append(buffer, static_cast<std::size_t>(n));
                }
                ::close(cfd);

                try
                {
                    auto message = data.empty() ? nlohmann::json::object() : nlohmann::json::parse(data);
                    if (!message.is_object())
                    {
                        message = nlohmann::json::object();
                    }
                    detail::post_to_ui_thread(m_impl->app.get(), [this, message] {
                        dispatchSecondInstance(message);
                    });
                }
                catch (...)
                {
                }
            }

            ::close(sfd);
            m_impl->instance_socket = -1;
            ::unlink(sock_path.c_str());
        });
    }

    // 第二个实例向主实例发送启动信息：{ "argv": [...], "cwd": "..." }。
    // 连接失败（主实例恰好退出/未就绪）时静默放弃，调用方仍按“非主实例”继续。
    void Application::notify_primary(const std::string &sock_path)
    {
        const int cfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (cfd < 0)
        {
            return;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(cfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            ::close(cfd);
            return;
        }

        nlohmann::json message;
        message["argv"] = current_argv();
        try
        {
            message["cwd"] = path::cwd();
        }
        catch (...)
        {
            message["cwd"] = "";
        }
        const auto data = message.dump();
        ::write(cfd, data.data(), data.size());
        ::close(cfd);
    }
#endif
} // namespace waty
