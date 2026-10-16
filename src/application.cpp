#include <waty/application.hpp>

#include "detail.hpp"

#include <saucer/app.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
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

namespace waty::detail
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
        std::fprintf(stderr, "[waty][%s] %.*s\n", std::string(level).c_str(),
                     static_cast<int>(message.size()), message.data());
    }
} // namespace waty::detail

namespace waty
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

        // 目录名严格校验（appName / organizationName 共用）：
        // 非空、1-64 字符、^[A-Za-z0-9_]+$，且不得为 Windows 保留设备名。
        bool is_valid_directory_name(std::string_view name)
        {
            if (name.empty() || name.size() > 64)
            {
                return false;
            }

            for (const unsigned char c : name)
            {
                const bool ok =
                    (c >= '0' && c <= '9') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    c == '_';
                if (!ok)
                {
                    return false;
                }
            }

            std::string upper;
            upper.reserve(name.size());
            for (const unsigned char c : name)
            {
                upper.push_back(static_cast<char>(std::toupper(c)));
            }

            static constexpr std::array<std::string_view, 22> reserved = {
                "CON", "PRN", "AUX", "NUL",
                "COM1", "COM2", "COM3", "COM4", "COM5",
                "COM6", "COM7", "COM8", "COM9",
                "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                "LPT6", "LPT7", "LPT8", "LPT9"
            };

            const std::string_view upper_view(upper);
            return std::find(reserved.begin(), reserved.end(), upper_view) == reserved.end();
        }
    } // namespace

    std::string detail::executable_path()
    {
#if defined(__linux__)
        char buffer[PATH_MAX];
        const auto n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (n > 0)
        {
            buffer[n] = '\0';
            return buffer;
        }
#endif
#if defined(_WIN32)
        // TODO(platform): GetModuleFileNameW 返回完整路径，需转 UTF-8。
#endif
#if defined(__APPLE__)
        // TODO(platform): _NSGetExecutablePath 返回完整路径。
#endif
        return {};
    }

    std::string detail::executable_dir()
    {
        const auto exe = detail::executable_path();
        if (exe.empty())
        {
            return std::filesystem::current_path().string();
        }
        return std::filesystem::path(exe).parent_path().string();
    }

    Application::Application(const ApplicationOptions &options)
        : m_impl(std::make_unique<Impl>())
    {
        if (!is_valid_directory_name(options.appName))
        {
            detail::log_message("error", "waty: ApplicationOptions.appName 非法: '" + std::string(options.appName) + "'");
            throw std::invalid_argument(
                "waty: ApplicationOptions.appName 必须非空、匹配 ^[A-Za-z0-9_]+$、长度不超过 64，"
                "且不得为 Windows 保留设备名（CON/PRN/AUX/NUL/COM1-9/LPT1-9）。"
                "实际值: " + std::string(options.appName));
        }
        if (!is_valid_directory_name(options.organizationName))
        {
            detail::log_message("error", "waty: ApplicationOptions.organizationName 非法: '" + std::string(options.organizationName) + "'");
            throw std::invalid_argument(
                "waty: ApplicationOptions.organizationName 必须非空、匹配 ^[A-Za-z0-9_]+$、长度不超过 64，"
                "且不得为 Windows 保留设备名（CON/PRN/AUX/NUL/COM1-9/LPT1-9）。"
                "实际值: " + std::string(options.organizationName));
        }

        m_impl->app_name          = std::string(options.appName);
        m_impl->organization_name = std::string(options.organizationName);
        m_impl->icon              = std::string(options.icon);
        m_impl->permissions = options.permissions;
        if (options.logFile)
        {
            m_impl->log_file = std::string(*options.logFile);
        }
        if (options.webviewStoragePath && !options.webviewStoragePath->empty())
        {
            m_impl->webview_storage_path = std::string(*options.webviewStoragePath);
        }

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
    }

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
        const std::string &name = m_impl->app_name;

#if defined(_WIN32)
        const char *home_env = std::getenv("USERPROFILE");
#else
        const char *home_env = std::getenv("HOME");
#endif
        const std::string home = home_env ? home_env : std::string{};

        const auto xdg_config = [&]() -> std::string {
#if defined(_WIN32)
            if (const char *v = std::getenv("APPDATA"); v && *v)
            {
                return v;
            }
            return home + "\\AppData\\Roaming";
#elif defined(__APPLE__)
            return home + "/Library/Preferences";
#else
            if (const char *v = std::getenv("XDG_CONFIG_HOME"); v && *v && v[0] == '/')
            {
                return v;
            }
            return home + "/.config";
#endif
        };
        const auto xdg_cache = [&]() -> std::string {
#if defined(_WIN32)
            if (const char *v = std::getenv("LOCALAPPDATA"); v && *v)
            {
                return v;
            }
            return home + "\\AppData\\Local";
#elif defined(__APPLE__)
            return home + "/Library/Caches";
#else
            if (const char *v = std::getenv("XDG_CACHE_HOME"); v && *v && v[0] == '/')
            {
                return v;
            }
            return home + "/.cache";
#endif
        };
        const auto xdg_data = [&]() -> std::string {
#if defined(_WIN32)
            if (const char *v = std::getenv("LOCALAPPDATA"); v && *v)
            {
                return v;
            }
            return home + "\\AppData\\Local";
#elif defined(__APPLE__)
            return home + "/Library/Application Support";
#else
            if (const char *v = std::getenv("XDG_DATA_HOME"); v && *v && v[0] == '/')
            {
                return v;
            }
            return home + "/.local/share";
#endif
        };

        const std::string &org = m_impl->organization_name;

        // 在基础目录下按「组织或发布者 / 应用名」组织用户数据，
        // 默认组织名为 waty；Windows 使用反斜杠，其余平台使用正斜杠。
#if defined(_WIN32)
        constexpr char k_path_sep = '\\';
#else
        constexpr char k_path_sep = '/';
#endif
        const auto join_app_path = [&](const std::string &base) -> std::string {
            return base + k_path_sep + org + k_path_sep + name;
        };

        switch (path)
        {
            case SysPath::AppData:    return join_app_path(xdg_data());
            case SysPath::Config:     return join_app_path(xdg_config());
            case SysPath::Temp: {
#if defined(_WIN32)
                if (const char *v = std::getenv("TEMP"); v && *v) return v;
                if (const char *v = std::getenv("TMP"); v && *v) return v;
                return home + "\\AppData\\Local\\Temp";
#else
                if (const char *v = std::getenv("TMPDIR"); v && *v)
                {
                    return v;
                }
                return "/tmp";
#endif
            }
            case SysPath::Downloads: return home + "/Downloads";
            case SysPath::Documents: return home + "/Documents";
            case SysPath::Executable: return detail::executable_path();
            case SysPath::Home:      return home;
            case SysPath::Cache:     return join_app_path(xdg_cache());
            case SysPath::Logs:      return join_app_path(xdg_config()) + k_path_sep + "logs";
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
        const std::string user_data = getPath(SysPath::Config);
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
} // namespace waty
