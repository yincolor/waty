#include <waty/web_window.hpp>

#include "detail.hpp"

#include <saucer/icon.hpp>
#include <saucer/scheme.hpp>
#include <saucer/webview.hpp>
#include <saucer/window.hpp>

#if defined(SAUCER_WEBKITGTK)
    #include <gtk/gtk.h>
    #include <saucer/modules/stable/webkitgtk.hpp>
#endif

#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace waty::detail
{
    namespace
    {
        // 使用 Meyers 单例，避免静态初始化顺序问题（waty_embed 生成的静态初始化器
        // 在程序启动早期调用 register_embedded_resource，此时全局 map 可能尚未构造）。
        std::mutex &embed_mutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::unordered_map<std::string, std::string> &embed_map()
        {
            static std::unordered_map<std::string, std::string> map;
            return map;
        }
    } // namespace

    void register_embedded_resource(std::string path, std::string content)
    {
        std::lock_guard<std::mutex> lock(embed_mutex());
        embed_map()[std::move(path)] = std::move(content);
    }

    std::optional<std::string> find_embedded_resource(std::string_view path)
    {
        std::lock_guard<std::mutex> lock(embed_mutex());
        auto &map = embed_map();
        const auto it = map.find(std::string(path));
        if (it == map.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<saucer::icon> resolve_icon(std::string_view path)
    {
        if (path.empty())
        {
            return std::nullopt;
        }

        // 1. 嵌入资源中的相对路径
        if (const auto content = find_embedded_resource(path))
        {
            const auto stash = saucer::stash::from_str(*content);
            if (auto icon = saucer::icon::from(stash))
            {
                return std::move(*icon);
            }
        }

        // 2. 相对于可执行文件目录的文件系统路径
        std::filesystem::path fs_path(path);
        if (fs_path.is_relative())
        {
            fs_path = std::filesystem::path(waty::detail::executable_dir()) / fs_path;
        }

        std::error_code ec;
        if (std::filesystem::exists(fs_path, ec))
        {
            if (auto icon = saucer::icon::from(fs_path))
            {
                return std::move(*icon);
            }
        }

        return std::nullopt;
    }

#if defined(SAUCER_WEBKITGTK)
    namespace
    {
        // saucer 在 GTK 后端的 window::set_icon 是空实现，窗口图标会显示为系统默认图标。
        // 这里直接通过 GTK4 的 GdkToplevel 设置窗口图标。
        void set_gtk_window_icon(GtkWindow *gtk_window, const saucer::icon &icon)
        {
            auto *surface = gtk_native_get_surface(GTK_NATIVE(gtk_window));
            if (!surface)
            {
                log_message("warn", "waty: 无法获取 GdkSurface，窗口图标设置失败");
                return;
            }

            auto *toplevel = GDK_TOPLEVEL(surface);

            // saucer::icon::data() 会把纹理重新编码为 PNG 字节，因此嵌入资源与文件系统图标都能覆盖。
            const auto png = icon.data();
            GBytes *bytes = g_bytes_new(png.data(), static_cast<gsize>(png.size()));

            GError *error = nullptr;
            GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
            g_bytes_unref(bytes);

            if (!texture)
            {
                log_message("warn",
                            "waty: 无法加载窗口图标: " + std::string(error ? error->message : "未知错误"));
                if (error)
                {
                    g_error_free(error);
                }
                return;
            }

            GList *icons = g_list_append(nullptr, texture);
            gdk_toplevel_set_icon_list(toplevel, icons);
            g_list_free(icons);
            g_object_unref(texture);
        }

        void on_window_realize(GtkWidget *widget, gpointer user_data)
        {
            auto *icon = static_cast<saucer::icon *>(user_data);
            if (!icon)
            {
                return;
            }
            set_gtk_window_icon(GTK_WINDOW(widget), *icon);
        }
    } // namespace
#endif

    void apply_window_icon(saucer::window &window, const saucer::icon &icon)
    {
#if defined(SAUCER_WEBKITGTK)
        auto *gtk_window = window.native<true>().window;
        if (!gtk_window)
        {
            log_message("warn", "waty: 无法获取 GtkWindow，窗口图标设置失败");
            return;
        }

        // 必须等 GTK 窗口 realize（底层 GdkSurface/XID 创建完成）后再设置，
        // 否则 gdk_toplevel_set_icon_list 会因 surface 未就绪而被忽略。
        if (gtk_widget_get_realized(GTK_WIDGET(gtk_window)))
        {
            set_gtk_window_icon(gtk_window, icon);
        }
        else
        {
            g_signal_connect_data(GTK_WIDGET(gtk_window), "realize", G_CALLBACK(+on_window_realize),
                                  new saucer::icon(icon),
                                  [](gpointer data, GClosure *) { delete static_cast<saucer::icon *>(data); },
                                  G_CONNECT_AFTER);
        }
#else
        window.set_icon(icon);
#endif
    }
} // namespace waty::detail

namespace waty
{
    namespace
    {
        const std::string g_default_index = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="utf-8">
    <title>waty</title>
    <style>
        body { font-family: system-ui, sans-serif; display: grid; place-items: center; height: 100vh; margin: 0; }
        main { text-align: center; }
        h1 { font-size: 3rem; margin: 0; }
        p { color: #555; }
    </style>
</head>
<body>
    <main>
        <h1>🚀 waty</h1>
        <p>waty 核心库运行成功</p>
    </main>
</body>
</html>
)html";

        void seed_default_embedded()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                // 仅在用户未通过 waty_embed 注册同名资源时，注入默认页。
                if (!detail::find_embedded_resource("index.html"))
                {
                    detail::register_embedded_resource("index.html", g_default_index);
                }
            });
        }

        std::string normalize_embedded_path(std::string_view entry)
        {
            std::filesystem::path path(entry);
            const auto normalized = path.lexically_normal().generic_string();

            if (normalized.empty())
            {
                throw std::invalid_argument("waty: loadFile 的 entry 不能为空");
            }
            if (normalized.front() == '/')
            {
                throw std::invalid_argument("waty: loadFile 的 entry 必须是相对路径: " + std::string(entry));
            }
            if (normalized == ".." || normalized.starts_with("../"))
            {
                throw std::invalid_argument("waty: loadFile 的路径越出嵌入根目录: " + std::string(entry));
            }
            return normalized;
        }

        std::string mime_for(std::string_view path)
        {
            const std::string ext = std::filesystem::path(path).extension().string();
            if (ext == ".html" || ext == ".htm") return "text/html";
            if (ext == ".css") return "text/css";
            if (ext == ".js" || ext == ".mjs") return "text/javascript";
            if (ext == ".json") return "application/json";
            if (ext == ".png") return "image/png";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".svg") return "image/svg+xml";
            if (ext == ".gif") return "image/gif";
            if (ext == ".ico") return "image/x-icon";
            if (ext == ".wasm") return "application/wasm";
            if (ext == ".woff") return "font/woff";
            if (ext == ".woff2") return "font/woff2";
            if (ext == ".ttf") return "font/ttf";
            if (ext == ".txt") return "text/plain";
            return "application/octet-stream";
        }

        const std::unordered_set<std::string> k_window_events = {
            "close", "closed", "resize", "move", "focus", "blur",
            "minimize", "maximize", "restore", "show", "hide", "sub-window-close",
        };
    } // namespace

    WebWindow::WebWindow(Application *app, WindowId id, const WebWindowOptions &options)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->app        = app;
        m_impl->id         = id;
        m_impl->permissions = app->m_impl->permissions;
        m_impl->title      = std::string(options.title);
        m_impl->width      = options.width;
        m_impl->height     = options.height;
        m_impl->min_width  = options.minWidth;
        m_impl->min_height = options.minHeight;
        m_impl->resizable  = options.resizable;
        m_impl->fullscreen = options.fullscreen;
        m_impl->frame      = options.frame;
        m_impl->parent     = options.parentWebWindow;
        m_impl->icon       = std::string(options.icon);

        // 解析 WebView 运行时数据目录：窗口覆盖 → 应用配置 → 默认平台数据目录。
        // 注意：Saucer 的 storage_path 在 WebKitGTK 后端是一个 SQLite 文件路径，
        // 在 WebView2 / Qt 后端则是目录路径，因此这里按平台拼接。
        const auto apply_explicit_storage = [&](std::string_view raw) {
            auto path = std::filesystem::path(std::string(raw));
            std::error_code ec;
            if (path.has_parent_path())
            {
                std::filesystem::create_directories(path.parent_path(), ec);
            }
            if (ec)
            {
                detail::log_message("warn", "waty: 无法创建 WebView 数据目录的父目录，回退 Saucer 默认: " +
                                             path.string() + " (" + ec.message() + ")");
                m_impl->storage_path.reset();
                return;
            }
            m_impl->storage_path = path.string();
            detail::log_message("info", "waty: WebView storage_path: " + path.string());
        };

        if (options.storagePath && !options.storagePath->empty())
        {
            apply_explicit_storage(*options.storagePath);
        }
        else if (app && app->m_impl->webview_storage_path && !app->m_impl->webview_storage_path->empty())
        {
            apply_explicit_storage(*app->m_impl->webview_storage_path);
        }
        else if (app)
        {
            const auto base = app->getPath(SysPath::AppData);
            auto data_dir   = std::filesystem::path(base) / "webview";

            std::error_code ec;
            std::filesystem::create_directories(data_dir, ec);
            if (ec)
            {
                detail::log_message("warn", "waty: 无法创建 WebView 数据目录，回退 Saucer 默认: " +
                                             data_dir.string() + " (" + ec.message() + ")");
                m_impl->storage_path.reset();
            }
            else
            {
#if defined(SAUCER_WEBKITGTK)
                // WebKitGTK 的 storage_path 是 Cookie 持久化 SQLite 文件路径。
                m_impl->storage_path = (data_dir / "cookies.sqlite").string();
#else
                m_impl->storage_path = data_dir.string();
#endif
                detail::log_message("info", "waty: WebView 数据目录: " + data_dir.string() +
                                             "，storage_path: " + *m_impl->storage_path);
            }
        }

        m_impl->cached_size = {options.width, options.height};
    }

    WebWindow::~WebWindow() = default;

    WindowId WebWindow::id() const
    {
        return m_impl->id;
    }

    void WebWindow::loadFile(std::string_view entry)
    {
        if (entry.empty())
        {
            throw std::invalid_argument("waty: loadFile 的 entry 不能为空");
        }
        const auto normalized = normalize_embedded_path(entry);

        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->view)
        {
            m_impl->view->set_url(std::string("embedded://root/") + normalized);
        }
        else
        {
            m_impl->pending_entry = normalized;
        }
    }

    void WebWindow::loadURL(std::string_view url)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->view)
        {
            m_impl->view->set_url(std::string(url));
        }
        else
        {
            m_impl->pending_url = std::string(url);
        }
    }

    void WebWindow::setFullscreen(bool fullscreen)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->fullscreen = fullscreen;
        if (m_impl->window)
        {
            m_impl->window->set_fullscreen(fullscreen);
        }
    }

    bool WebWindow::isFullscreen() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->fullscreen;
    }

    void WebWindow::setSize(int width, int height)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->cached_size = {width, height};
        m_impl->window->set_size({width, height});
    }

    void WebWindow::setMinSize(int width, int height)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->min_width  = width;
        m_impl->min_height = height;
        m_impl->window->set_min_size({width, height});
    }

    void WebWindow::setResizable(bool resizable)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->resizable = resizable;
        if (m_impl->window)
        {
            m_impl->window->set_resizable(resizable);
        }
    }

    std::pair<int, int> WebWindow::getSize() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->cached_size;
    }

    void WebWindow::setPosition(int x, int y)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->cached_position = {x, y};
        m_impl->window->set_position({x, y});
        m_impl->window_events.emit("move", nlohmann::json{{"x", x}, {"y", y}});
    }

    std::pair<int, int> WebWindow::getPosition() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->cached_position;
    }

    void WebWindow::center()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window || !m_impl->saucer_app) throw std::logic_error("waty: 窗口尚未创建");

        const auto screens = m_impl->saucer_app->screens();
        if (screens.empty())
        {
            return;
        }
        const auto &screen = screens.front();
        const int x = screen.position.x + (screen.size.w - m_impl->cached_size.first) / 2;
        const int y = screen.position.y + (screen.size.h - m_impl->cached_size.second) / 2;
        m_impl->cached_position = {x, y};
        m_impl->window->set_position({x, y});
    }

    void WebWindow::maximize()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->window->set_maximized(true);
    }

    void WebWindow::minimize()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->window->set_minimized(true);
    }

    void WebWindow::restore()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->window->set_maximized(false);
        m_impl->window->set_minimized(false);
    }

    void WebWindow::close()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->window)
        {
            m_impl->window->close();
        }
    }

    void WebWindow::hide()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->cached_visible = false;
        if (m_impl->window)
        {
            m_impl->window->hide();
        }
        m_impl->window_events.emit("hide");
    }

    void WebWindow::show()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->cached_visible = true;
        if (m_impl->window)
        {
            m_impl->window->show();
            m_impl->window_events.emit("show");
        }
        else
        {
            m_impl->pending_show = true;
        }
    }

    void WebWindow::focus()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->window->focus();
    }

    void WebWindow::setAlwaysOnTop(bool enabled)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->window) throw std::logic_error("waty: 窗口尚未创建");
        m_impl->window->set_always_on_top(enabled);
    }

    void WebWindow::setDecorated(bool enabled)
    {
        // 兼容旧接口：与 setWindowFrameState 等价
        setWindowFrameState(enabled);
    }

    void WebWindow::setWindowFrameState(bool frame)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->frame = frame;
        if (m_impl->window)
        {
            // frame=false 时使用 decoration::partial（而非 none）：去除标题栏/边框，
            // 但保留窗口阴影与调整大小的能力。
            m_impl->window->set_decorations(frame ? saucer::window::decoration::full : saucer::window::decoration::partial);
        }
    }

    bool WebWindow::hasWindowFrame() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->frame;
    }

    void WebWindow::setTitle(std::string_view title)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->title = std::string(title);
        if (m_impl->window)
        {
            m_impl->window->set_title(m_impl->title);
        }
    }

    std::string WebWindow::title() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->title;
    }

    bool WebWindow::isMaximized() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->cached_maximized;
    }

    bool WebWindow::isMinimized() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->cached_minimized;
    }

    bool WebWindow::isVisible() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->cached_visible;
    }

    Subscription WebWindow::windowEventHandle(std::string_view event,
                                              std::function<void(Event &, const nlohmann::json &)> callback)
    {
        if (!k_window_events.contains(std::string(event)))
        {
            throw std::invalid_argument("waty: 未知窗口事件: " + std::string(event));
        }
        return m_impl->window_events.on(std::string(event), std::move(callback));
    }

    void WebWindow::realize(saucer::application *app)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->realized)
        {
            return;
        }
        m_impl->realized   = true;
        m_impl->saucer_app = app;

        // 创建原生窗口
        auto window = saucer::window::create(app);
        if (!window)
        {
            throw std::runtime_error("waty: 创建原生窗口失败: " + window.error().message());
        }
        m_impl->window = std::move(*window);

        if (!m_impl->title.empty())
        {
            m_impl->window->set_title(m_impl->title);
        }
        m_impl->window->set_size({m_impl->width, m_impl->height});
        if (m_impl->min_width > 0 || m_impl->min_height > 0)
        {
            m_impl->window->set_min_size({m_impl->min_width, m_impl->min_height});
        }
        m_impl->window->set_resizable(m_impl->resizable);
        m_impl->window->set_fullscreen(m_impl->fullscreen);
        m_impl->window->set_decorations(m_impl->frame ? saucer::window::decoration::full : saucer::window::decoration::partial);

        // 应用图标：优先窗口图标，否则应用图标（文档 6.4 / 5）
        std::optional<saucer::icon> window_icon;
        if (!m_impl->icon.empty())
        {
            window_icon = detail::resolve_icon(m_impl->icon);
        }
        else if (m_impl->app && m_impl->app->m_impl->resolved_icon)
        {
            window_icon = m_impl->app->m_impl->resolved_icon;
        }
        if (window_icon)
        {
            detail::apply_window_icon(*m_impl->window, *window_icon);
        }
        // TODO(phase2): 父窗口（parent）、初始位置

        // 创建 WebView
        saucer::webview::options webview_options{.window = m_impl->window};
        if (m_impl->storage_path)
        {
            webview_options.storage_path = std::filesystem::path(*m_impl->storage_path);
        }

        auto view = saucer::webview::create(webview_options);
        if (!view)
        {
            throw std::runtime_error("waty: 创建 WebView 失败: " + view.error().message());
        }
        m_impl->view = std::make_unique<saucer::webview>(std::move(*view));

        // embedded:// 协议处理
        seed_default_embedded();
        m_impl->view->handle_scheme("embedded", [](saucer::scheme::request request, saucer::scheme::executor executor) {
            auto path = request.url().path().string();
            if (!path.empty() && path.front() == '/')
            {
                path.erase(0, 1);
            }

            const auto content = detail::find_embedded_resource(path);
            if (!content)
            {
                executor.reject(saucer::scheme::error::not_found);
                return;
            }

            saucer::scheme::response response{
                .data   = saucer::stash::from_str(*content),
                .mime   = mime_for(path),
                .status = 200,
            };
            executor.resolve(std::move(response));
        });

        // 原生窗口事件 → waty 窗口事件
        m_impl->window->on<saucer::window::event::close>({{.func = [this]() -> saucer::policy {
            return m_impl->window_events.emit("close") ? saucer::policy::block : saucer::policy::allow;
        }, .clearable = false}});

        m_impl->window->on<saucer::window::event::closed>({{.func = [this] {
            m_impl->cached_visible = false;
            m_impl->window_events.emit("closed");

            // 通知父窗口 sub-window-close（文档 7.5）
            if (auto parent = m_impl->parent.lock())
            {
                parent->m_impl->window_events.emit("sub-window-close", nlohmann::json{{"windowId", m_impl->id}});
            }

            // 通知 Application 更新 window-all-closed
            if (m_impl->app)
            {
                m_impl->app->handleWindowClosed();
            }
        }, .clearable = false}});

        m_impl->window->on<saucer::window::event::resize>({{.func = [this](int w, int h) {
            m_impl->cached_size = {w, h};
            m_impl->window_events.emit("resize", nlohmann::json{{"width", w}, {"height", h}});
        }, .clearable = false}});

        m_impl->window->on<saucer::window::event::focus>({{.func = [this](bool focused) {
            m_impl->window_events.emit(focused ? "focus" : "blur");
        }, .clearable = false}});

        m_impl->window->on<saucer::window::event::maximize>({{.func = [this](bool maximized) {
            m_impl->cached_maximized = maximized;
            m_impl->window_events.emit(maximized ? "maximize" : "restore");
        }, .clearable = false}});

        m_impl->window->on<saucer::window::event::minimize>({{.func = [this](bool minimized) {
            m_impl->cached_minimized = minimized;
            m_impl->window_events.emit(minimized ? "minimize" : "restore");
        }, .clearable = false}});

        // JS 桥（ipc / os / fs / window / dialog）
        detail::setup_js_bridge(*m_impl);

        // 应用排队中的加载与显示
        if (m_impl->pending_url)
        {
            m_impl->view->set_url(std::string(*m_impl->pending_url));
            m_impl->pending_url.reset();
        }
        else if (m_impl->pending_entry)
        {
            m_impl->view->set_url(std::string("embedded://root/") + *m_impl->pending_entry);
            m_impl->pending_entry.reset();
        }

        if (m_impl->pending_show)
        {
            m_impl->window->show();
            m_impl->pending_show = false;
            m_impl->cached_visible = true;
            m_impl->window_events.emit("show");
        }
    }

    void WebWindow::openDevTools()
    {
        if (!m_impl->app || !m_impl->app->m_impl->permissions.devtools)
        {
            throw std::runtime_error("waty: DevTools 未启用（permissions.devtools=false）");
        }
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->view)
        {
            m_impl->view->set_dev_tools(true);
        }
    }
} // namespace waty
