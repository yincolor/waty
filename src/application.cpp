#include <whiz/application.hpp>

#include "detail.hpp"

#include <saucer/app.hpp>

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
    #include <limits.h>
    #include <sys/file.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

namespace whiz::detail
{
    void post_to_ui_thread(saucer::application *app, std::function<void()> callback)
    {
        if (!app)
        {
            return;
        }
        app->post(std::move(callback));
    }

    void log_message(std::string_view level, std::string_view message)
    {
        // TODO(doc-gap): 阶段 2 仅控制台输出；ApplicationOptions.logFile 写入后续补齐。
        std::fprintf(stderr, "[whiz][%s] %.*s\n", std::string(level).c_str(),
                     static_cast<int>(message.size()), message.data());
    }
} // namespace whiz::detail

namespace whiz
{
    namespace
    {
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

        std::string current_working_dir()
        {
            char buffer[PATH_MAX];
            if (::getcwd(buffer, sizeof(buffer)) != nullptr)
            {
                return buffer;
            }
            return {};
        }

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

    std::string detail::executable_dir()
    {
#if defined(__linux__)
        char buffer[PATH_MAX];
        const auto n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (n > 0)
        {
            buffer[n] = '\0';
            return std::filesystem::path(buffer).parent_path().string();
        }
#endif
        return std::filesystem::current_path().string();
    }

    Application::Application(const ApplicationOptions &options)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->app_name    = std::string(options.appName);
        m_impl->icon        = std::string(options.icon);
        m_impl->permissions = options.permissions;
        if (options.logFile)
        {
            m_impl->log_file = std::string(*options.logFile);
        }

        std::string id = m_impl->app_name;
        if (id.empty())
        {
            id = "whiz-app";  // TODO(doc-gap): 文档未规定 appName 为空时的行为
        }

        // quit_on_last_window_closed 设为 false：由 whiz 自行处理 window-all-closed 默认行为，
        // 以保证 before-quit / will-quit / quit 生命周期事件完整触发。
        auto created = saucer::application::create({
            .id                       = id,
            .quit_on_last_window_closed = false,
        });
        if (!created)
        {
            throw std::runtime_error("whiz: 创建 saucer::application 失败: " + created.error().message());
        }
        m_impl->app = std::make_unique<saucer::application>(std::move(*created));

        // 预解析应用图标（先查嵌入资源，再查可执行文件目录）
        if (!m_impl->icon.empty())
        {
            m_impl->resolved_icon = detail::resolve_icon(m_impl->icon);
            if (!m_impl->resolved_icon)
            {
                detail::log_message("warn", "whiz: ApplicationOptions.icon 未找到，使用系统默认图标: " + m_impl->icon);
            }
        }

        // 挂接 saucer 的 quit 事件：映射为 whiz 的 before-quit / will-quit。
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
    }

    void Application::setIcon(std::string_view iconPath)
    {
        m_impl->icon          = std::string(iconPath);
        m_impl->resolved_icon = detail::resolve_icon(m_impl->icon);
        if (!m_impl->resolved_icon)
        {
            detail::log_message("warn", "whiz: setIcon 未找到图标，使用系统默认图标: " + m_impl->icon);
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
            throw std::invalid_argument("whiz: 窗口宽度/高度/最小宽度/最小高度不能为负值");
        }
        // TODO(phase2): 宽度/高度/最小尺寸超过屏幕分辨率 → std::out_of_range

        const WindowId id = m_impl->next_id++;
        std::shared_ptr<WebWindow> window(new WebWindow(this, id, options));

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
            throw std::invalid_argument("whiz: 未知应用事件: " + std::string(event));
        }
        return m_impl->events.on(std::string(event), std::move(callback));
    }

    Subscription Application::onEventLoopTick(std::function<void()> callback)
    {
        auto task = std::make_shared<Impl::TickTask>();
        task->callback = std::move(callback);

        {
            std::lock_guard<std::mutex> lock(m_impl->tick_mutex);
            if (m_impl->tick_iterating)
            {
                // 文档 7.3.3：回调中禁止注册/取消任务
                assert(false && "whiz: onEventLoopTick 回调中禁止注册任务");
                std::abort();
            }
            m_impl->tick_tasks.push_back(task);
        }

        start_tick_chain();

        return Subscription([task] {
            task->alive.store(false);
        });
    }

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

    void Application::run_tick_loop()
    {
        if (m_impl->tick_iterating)
        {
            assert(false && "whiz: onEventLoopTick 重入");
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
        m_impl->app->quit();
    }

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

    std::string Application::getPath(SysPath path) const
    {
        // TODO(doc-gap): 路径约定参考 Electron/Node.js（Linux）。
        const char *home_env = std::getenv("HOME");
        const std::string home = home_env ? home_env : std::string{};
        const std::string &name = m_impl->app_name;

        const auto xdg_config = [&]() -> std::string {
            if (const char *v = std::getenv("XDG_CONFIG_HOME"); v && *v)
            {
                return v;
            }
            return home + "/.config";
        };
        const auto xdg_cache = [&]() -> std::string {
            if (const char *v = std::getenv("XDG_CACHE_HOME"); v && *v)
            {
                return v;
            }
            return home + "/.cache";
        };

        switch (path)
        {
            case SysPath::UserData:  return xdg_config() + "/" + name;
            case SysPath::Temp: {
                if (const char *v = std::getenv("TMPDIR"); v && *v)
                {
                    return v;
                }
                return "/tmp";
            }
            case SysPath::Downloads: return home + "/Downloads";
            case SysPath::Documents: return home + "/Documents";
            case SysPath::AppDir:    return detail::executable_dir();
            case SysPath::Home:      return home;
            case SysPath::Cache:     return xdg_cache() + "/" + name;
            case SysPath::Logs:      return xdg_config() + "/" + name + "/logs";
        }
        return {};
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

    bool Application::requestSingleInstanceLock()
    {
        if (m_impl->single_instance)
        {
            return true;
        }

#if defined(__linux__)
        const std::string user_data = getPath(SysPath::UserData);
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

    void Application::dispatchSecondInstance(nlohmann::json data)
    {
        data["windowId"] = [&] {
            std::lock_guard<std::mutex> lock(m_impl->windows_mutex);
            return m_impl->windows.empty() ? WindowId{0} : m_impl->windows.front()->id();
        }();
        m_impl->events.emit("second-instance", data);
    }

#if defined(__linux__)
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
        message["cwd"]  = current_working_dir();
        const auto data = message.dump();
        ::write(cfd, data.data(), data.size());
        ::close(cfd);
    }
#endif
} // namespace whiz
