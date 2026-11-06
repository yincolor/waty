// 窗口 + WebView 实现（见 DESIGN.md §7.4）。
//
// 本文件包含三部分：
//   1. 嵌入资源注册表与图标解析（waty_embed 生成代码的运行时支撑）；
//   2. WebWindow 的窗口状态/控制 API（realize 前后行为差异见各方法注释）；
//   3. realize()：在 UI 线程创建原生窗口/WebView，注册 embedded:// 协议，
//      并把 Saucer 原生事件映射为 waty 窗口事件，最后注入 JS 桥。
//
// realize 前后行为：
//   * 可缓存的配置（标题/尺寸/最小尺寸/边框/全屏/可缩放/图标/存储目录）在构造时保存；
//   * 可排队的操作（loadFile / loadURL / show）写入 pending_*，realize 后立即执行；
//   * 无法缓存的控制（setSize / maximize / ...）在 realize 前抛 std::logic_error。
//
// 嵌入资源路径约定：相对路径、以 '/' 分隔；loadFile 会做词法规范化并拒绝越界路径。

#include <waty/web_window.hpp>

#include "detail.hpp"
#include "sqlite3_service.hpp"

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
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace waty::detail
{
    namespace
    {
        // 嵌入资源注册表。
        // 使用 Meyers 单例，避免静态初始化顺序问题（waty_embed 生成的静态初始化器
        // 在程序启动早期调用 register_embedded_resource，此时全局 map 可能尚未构造）。
        // 互斥锁保护 map；查找返回拷贝（std::optional<std::string>），避免调用方持有悬空引用。
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

    // 图标解析顺序（文档 5 / 6.4）：
    //   1. 嵌入资源中同名相对路径（如 waty_embed 打包的 "icon.png"）；
    //   2. 文件系统路径；相对路径基于可执行文件目录解析。
    // 两者都失败返回 nullopt，调用方回退系统默认图标。
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
        // 注意：必须在 GDK surface realize 之后调用，否则 icon list 会被忽略。
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
        // 默认嵌入页：仅当用户没有通过 waty_embed 注册 index.html 时注入，
        // 方便最小示例在没有任何前端产物时也能跑通。
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

        // 规范化嵌入资源路径并做安全校验：
        //   * 必须非空；
        //   * 必须是相对路径（禁止以 '/' 开头）；
        //   * 规范化后不得为 ".." 或以 "../" 开头（防止读穿嵌入根目录）。
        // 返回使用 generic_string()（'/' 分隔），作为 embedded:// 的路径部分。
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

        // 根据扩展名推断 embedded:// 响应的 Content-Type。
        // 前端构建产物常见类型均已覆盖；未知类型回退 application/octet-stream。
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

        // 窗口事件白名单（windowEventHandle 只接受这些事件名）。
        // 其中 close 支持 preventDefault()；sub-window-close 由子窗口触发、发给父窗口。
        const std::unordered_set<std::string> k_window_events = {
            "close", "closed", "resize", "move", "focus", "blur",
            "minimize", "maximize", "restore", "show", "hide", "sub-window-close",
        };
    } // namespace

    // 构造函数只保存配置，不碰任何原生句柄（全部在 realize() 创建）。
    // 字符串字段从 string_view 拷贝，调用方无需保证生命周期。
    WebWindow::WebWindow(Application *app, WindowId id, const WebWindowOptions &options)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->app        = app;
        m_impl->id         = id;
        m_impl->permissions       = app->m_impl->permissions;
        m_impl->app_name          = app->m_impl->app_name;
        m_impl->organization_name = app->m_impl->organization_name;
        m_impl->title             = std::string(options.title);
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
            const auto base = app->m_impl->app_data_dir;
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

        // 查询类接口的初始值：构造参数即期望值，之后由原生事件更新。
        m_impl->cached_size = {options.width, options.height};
    }

    WebWindow::~WebWindow()
    {
        if (m_impl && m_impl->cwd_subscription.valid())
        {
            m_impl->cwd_subscription.unsubscribe();
        }

        // 窗口对象销毁：注销该窗口的全部 SQLite 句柄；若某个库已无其他窗口持有，
        // 连接会在 release_all 内部排空队列后关闭。应用退出时此处是最后一道兜底。
        if (m_impl && m_impl->sqlite3)
        {
            m_impl->sqlite3->release_all(m_impl->id);
        }
    }

    WindowId WebWindow::id() const
    {
        return m_impl->id;
    }

    // 加载嵌入资源。realize 前只记录 pending_entry；realize 后再 set_url。
    // 真正的内容由 realize 中注册的 embedded:// scheme handler 提供。
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

    // 加载远程 URL（完全交给 Saucer/WebView 默认策略，不额外限制）。
    // 与 loadFile 共用 pending 机制：realize 前调用会排队。
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

    // ---- 以下窗口控制大多要求 native window 已存在 ----
    // 会抛 std::logic_error：setSize / setMinSize / setPosition / center /
    //   maximize / minimize / restore / focus / setAlwaysOnTop；
    // 可缓存或排队：setTitle / setResizable / setFullscreen / setWindowFrameState /
    //   loadFile / loadURL / show；close / hide 在 realize 前为 no-op。
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
        // 兼容旧接口：与 setWindowFrameState 等价。
        // 历史上 decorated 表示“是否有标题栏/边框”，语义与 frame 一致。
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

    // 注册窗口事件（白名单校验）。
    // 与 Application::on 一致：同一事件名允许多个监听器，Subscription 需显式取消。
    Subscription WebWindow::windowEventHandle(std::string_view event,
                                              std::function<void(Event &, const nlohmann::json &)> callback)
    {
        if (!k_window_events.contains(std::string(event)))
        {
            throw std::invalid_argument("waty: 未知窗口事件: " + std::string(event));
        }
        return m_impl->window_events.on(std::string(event), std::move(callback));
    }

    // 在 UI 线程创建原生窗口与 WebView，并把所有排队操作应用上去。
    //
    // 执行顺序（有依赖关系，不要随意调整）：
    //   1. 创建 saucer::window，应用标题/尺寸/最小尺寸/可缩放/全屏/边框/图标；
    //   2. 创建 webview，绑定 window 与存储目录；
    //   3. 注册 embedded:// scheme handler；
    //   4. 注册原生窗口事件 → waty 事件；
    //   5. 注入 JS 桥（依赖 webview 已存在）；
    //   6. 应用 pending 的 loadURL/loadFile 与 show。
    // realize 只执行一次（realized 标志），重复调用直接返回。
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

        // embedded:// 协议处理：URL 的 path 去掉前导 '/' 后作为资源 key。
        // - 未找到：返回 404（not_found），前端可在 onerror 中处理；
        // - 找到：返回 200 + 根据扩展名推断的 MIME。
        // 注意这里不再重复路径越界校验：loadFile 已规范化，且 scheme 只能读到注册表内容。
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

        // 原生窗口事件 → waty 窗口事件映射：
        //   close      → "close"（可 preventDefault 阻止）
        //   closed     → "closed" + 父窗口 "sub-window-close" + 注销 SQLite + window-all-closed
        //   resize     → "resize" {width,height}（同时更新缓存）
        //   focus      → "focus"/"blur"
        //   maximize   → "maximize"/"restore"（更新缓存）
        //   minimize   → "minimize"/"restore"（更新缓存）
        // 所有监听器 clearable=false：窗口生命周期内不允许 Saucer 自行清除。
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

            // 窗口关闭：注销该窗口持有的 SQLite 句柄。
            // 若这是最后一个使用者，release_all 会排空队列并 join 工作线程（可能耗时），
            // 因此放到后台线程，避免阻塞 UI 线程。
            if (m_impl->sqlite3)
            {
                auto service = m_impl->sqlite3;
                const auto window = m_impl->id;
                std::thread([service = std::move(service), window] { service->release_all(window); }).detach();
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

        // JS 桥（ipc / os / path / window / fs / dialog / sqlite3 / notify / log）。
        // 只在 realize 时注入一次；脚本内容由权限位决定（见 js_bridge.cpp）。
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

    // 打开 DevTools（需 permissions.devtools）。
    // JS 侧调用 waty.window.openDevTools() 时也会走到这里（保证权限校验只有一处）。
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
