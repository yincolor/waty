#include <whiz/web_window.hpp>

#include "detail.hpp"

#include <saucer/webview.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace whiz::detail
{
    namespace
    {
        // JS 桥脚本模板。@@XXX@@ 为权限占位符（"true"/"false"）。
        const char *k_bridge_script = R"js((function() {
    if (window.__whizInit) return;
    window.__whizInit = true;
    window.__whizSeq = 0;
    window.__whizPending = {};
    window.whiz = window.whiz || {};
    window.whiz.__listeners = {};

    window.__whizResolve = (id, value) => {
        const p = window.__whizPending[id];
        if (p) { delete window.__whizPending[id]; p.resolve(value); }
    };
    window.__whizReject = (id, error) => {
        const p = window.__whizPending[id];
        if (p) { delete window.__whizPending[id]; p.reject(error); }
    };
    window.__whizEmit = (event, data) => {
        const cbs = window.whiz.__listeners[event];
        if (cbs) cbs.slice().forEach(cb => { try { cb(data); } catch (e) {} });
    };

    const b64FromBytes = (bytes) => {
        let binary = "";
        for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
        return btoa(binary);
    };
    const bytesFromB64 = (b64) => {
        const binary = atob(b64);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
        return bytes;
    };

    const request = (channel, op, extra) => {
        return new Promise((resolve, reject) => {
            const id = ++window.__whizSeq;
            window.__whizPending[id] = { resolve, reject };
            const msg = Object.assign({ __whiz: channel, id }, extra || {});
            if (op !== undefined && op !== null) msg.op = op;
            window.saucer.internal.message(JSON.stringify(msg));
        });
    };

    if (@@IPC@@) {
        const makeInvoke = (options) => (event, ...args) => {
            return new Promise((resolve, reject) => {
                const id = ++window.__whizSeq;
                window.__whizPending[id] = { resolve, reject };
                if (options && options.timeout != null) {
                    setTimeout(() => {
                        if (window.__whizPending[id]) {
                            delete window.__whizPending[id];
                            reject({ name: "WhizIpcError", code: "IPC_TIMEOUT", message: "IPC request timed out" });
                        }
                    }, options.timeout);
                }
                if (options && options.signal) {
                    const abort = () => {
                        if (window.__whizPending[id]) {
                            delete window.__whizPending[id];
                            reject({ name: "WhizIpcError", code: "IPC_CANCELLED", message: "IPC request cancelled" });
                        }
                    };
                    if (options.signal.aborted) { abort(); return; }
                    options.signal.addEventListener("abort", abort, { once: true });
                }
                window.saucer.internal.message(JSON.stringify({ __whiz: "ipc", id, event, args }));
            });
        };
        window.whiz.ipc = {
            invoke: makeInvoke(),
            createInvoker: (options) => makeInvoker(options),
            on: (event, callback) => {
                if (!window.whiz.__listeners[event]) window.whiz.__listeners[event] = [];
                window.whiz.__listeners[event].push(callback);
                return () => {
                    const list = window.whiz.__listeners[event];
                    if (list) {
                        const i = list.indexOf(callback);
                        if (i >= 0) list.splice(i, 1);
                    }
                };
            }
        };
    }

    if (@@OS@@) {
        window.whiz.os = {
            platform: () => request("os", "platform"),
            arch: () => request("os", "arch"),
            homedir: () => request("os", "homedir"),
            getPath: (p) => request("os", "getPath", { path: p })
        };
    }

    if (@@WINDOW@@) {
        const w = (op, args) => request("window", op, args ? { args } : {});
        window.whiz.window = {
            setTitle: (t) => w("setTitle", [t]),
            getSize: () => w("getSize"),
            setPosition: (x, y) => w("setPosition", [x, y]),
            center: () => w("center"),
            maximize: () => w("maximize"),
            minimize: () => w("minimize"),
            restore: () => w("restore"),
            close: () => w("close"),
            hide: () => w("hide"),
            show: () => w("show"),
            focus: () => w("focus"),
            setAlwaysOnTop: (b) => w("setAlwaysOnTop", [b]),
            setDecorated: (b) => w("setDecorated", [b]),
            openDevTools: () => w("openDevTools"),
            isMaximized: () => w("isMaximized"),
            isMinimized: () => w("isMinimized"),
            isVisible: () => w("isVisible")
        };
    }

    if (@@FS_READ@@ || @@FS_WRITE@@) {
        window.whiz.fs = {};
        if (@@FS_READ@@) {
            window.whiz.fs.readFile = async (path) => {
                const r = await request("fs", "readFile", { path });
                return bytesFromB64(r.base64);
            };
            window.whiz.fs.stat = (path) => request("fs", "stat", { path });
            window.whiz.fs.exists = (path) => request("fs", "exists", { path });
            window.whiz.fs.createReadStream = async (path, options) => {
                const opened = await request("fs", "openReadStream", { path, highWaterMark: (options && options.highWaterMark) || 65536 });
                return new ReadableStream({
                    async pull(controller) {
                        const chunk = await request("fs", "readStreamChunk", { streamId: opened.streamId, size: opened.size });
                        if (chunk.eof) {
                            await request("fs", "closeStream", { streamId: opened.streamId });
                            controller.close();
                        } else {
                            controller.enqueue(bytesFromB64(chunk.base64));
                        }
                    },
                    cancel() { return request("fs", "closeStream", { streamId: opened.streamId }); }
                });
            };
        }
        if (@@FS_WRITE@@) {
            window.whiz.fs.writeFile = async (path, data) => {
                await request("fs", "writeFile", { path, base64: b64FromBytes(data) });
            };
            window.whiz.fs.createWriteStream = async (path) => {
                const opened = await request("fs", "openWriteStream", { path });
                return new WritableStream({
                    async write(chunk) {
                        await request("fs", "writeStreamChunk", { streamId: opened.streamId, base64: b64FromBytes(chunk) });
                    },
                    async close() { await request("fs", "closeStream", { streamId: opened.streamId }); },
                    async abort() { await request("fs", "closeStream", { streamId: opened.streamId }); }
                });
            };
        }
    }

    if (@@DIALOG@@) {
        window.whiz.dialog = {
            showOpenDialog: (options) => request("dialog", "showOpenDialog", { options: options || {} }),
            showSaveDialog: (options) => request("dialog", "showSaveDialog", { options: options || {} }),
            showMessageBox: (options) => request("dialog", "showMessageBox", { options })
        };
    }

    window.addEventListener("error", (e) => {
        window.saucer.internal.message(JSON.stringify({ __whiz: "jserror", message: e.message, source: e.filename, lineno: e.lineno, colno: e.colno }));
    });
    window.addEventListener("unhandledrejection", (e) => {
        let reason = e.reason;
        if (reason instanceof Error) reason = reason.message;
        window.saucer.internal.message(JSON.stringify({ __whiz: "jserror", message: String(reason), source: "", lineno: 0, colno: 0 }));
    });

    if (@@LOG@@) {
        const sendLog = (level, args) => {
            try {
                window.saucer.internal.message(JSON.stringify({ __whiz: "console", level, args: Array.from(args).map(a => (a instanceof Error ? a.message : String(a))) }));
            } catch (e) {}
        };
        const olog = console.log, owarn = console.warn, oerror = console.error;
        console.log = (...a) => { sendLog("log", a); olog.apply(console, a); };
        console.warn = (...a) => { sendLog("warn", a); owarn.apply(console, a); };
        console.error = (...a) => { sendLog("error", a); oerror.apply(console, a); };
    }
})();
)js";

        std::string replace_all(std::string s, std::string_view from, std::string_view to)
        {
            std::size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos)
            {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        }

        std::string build_js_script(const WebWindow::Impl &impl)
        {
            std::string script = k_bridge_script;
            const auto &p = impl.permissions;
            script = replace_all(script, "@@IPC@@", p.ipc ? "true" : "false");
            script = replace_all(script, "@@OS@@", p.os ? "true" : "false");
            script = replace_all(script, "@@WINDOW@@", p.window ? "true" : "false");
            script = replace_all(script, "@@FS_READ@@", p.fs.read ? "true" : "false");
            script = replace_all(script, "@@FS_WRITE@@", p.fs.write ? "true" : "false");
            script = replace_all(script, "@@DIALOG@@", p.dialog ? "true" : "false");
            script = replace_all(script, "@@LOG@@", p.log ? "true" : "false");
            return script;
        }

        nlohmann::json ipc_error_from_exception(const std::exception_ptr &ptr)
        {
            try
            {
                std::rethrow_exception(ptr);
            }
            catch (const std::invalid_argument &ex)
            {
                return nlohmann::json{{"name", "WhizIpcError"}, {"code", "IPC_INVALID_ARGUMENT"}, {"message", ex.what()}};
            }
            catch (const std::exception &ex)
            {
                return nlohmann::json{{"name", "WhizIpcError"}, {"code", "IPC_HANDLER_EXCEPTION"}, {"message", ex.what()}};
            }
            catch (...)
            {
                return nlohmann::json{{"name", "WhizIpcError"}, {"code", "IPC_HANDLER_EXCEPTION"}, {"message", "Unknown exception"}};
            }
        }

        void handle_ipc_request(WebWindow::Impl &impl, const nlohmann::json &req)
        {
            const std::uint64_t id = req.value("id", static_cast<std::uint64_t>(0));
            const std::string event = req.value("event", "");
            const nlohmann::json args = req.value("args", nlohmann::json::array());

            std::shared_ptr<WebWindow::Impl::IpcHandlerEntry> handler;
            {
                std::lock_guard<std::mutex> lock(impl.ipc_mutex);
                const auto it = impl.ipc_handlers.find(event);
                if (it == impl.ipc_handlers.end())
                {
                    respond_error(impl, id, nlohmann::json{{"name", "WhizIpcError"}, {"code", "IPC_HANDLER_NOT_FOUND"},
                                                           {"message", "No handler registered for '" + event + "'"}});
                    return;
                }

                // 惰性清理已通过 unsubscribe() 取消的 handler，再取第一个存活者
                auto &handlers = it->second;
                std::erase_if(handlers, [](const auto &h) {
                    std::lock_guard<std::mutex> lock(h->mutex);
                    return !h->alive;
                });
                if (handlers.empty())
                {
                    respond_error(impl, id, nlohmann::json{{"name", "WhizIpcError"}, {"code", "IPC_HANDLER_NOT_FOUND"},
                                                           {"message", "No handler registered for '" + event + "'"}});
                    return;
                }
                handler = handlers.front();  // TODO(doc-gap): 多个 handler 的调度策略
            }

            const auto saucer_app = impl.saucer_app;
            WebWindow::Impl *impl_ptr = &impl;

            // 在线程池中执行用户回调（返回 std::future<json>）
            std::thread([handler, args, id, impl_ptr, saucer_app] {
                try
                {
                    auto future = handler->callback(args);
                    const auto result = future.get();
                    post_to_ui_thread(saucer_app, [impl_ptr, id, result] {
                        respond(*impl_ptr, id, result);
                    });
                }
                catch (...)
                {
                    const auto error = ipc_error_from_exception(std::current_exception());
                    post_to_ui_thread(saucer_app, [impl_ptr, id, error] {
                        respond_error(*impl_ptr, id, error);
                    });
                }
            }).detach();
        }

        void handle_os_request(WebWindow::Impl &impl, const nlohmann::json &req)
        {
            const std::uint64_t id = req.value("id", static_cast<std::uint64_t>(0));
            const std::string op = req.value("op", "");

            if (op == "platform")
            {
#if defined(_WIN32)
                respond(impl, id, "win32");
#elif defined(__APPLE__)
                respond(impl, id, "darwin");
#else
                respond(impl, id, "linux");
#endif
                return;
            }
            if (op == "arch")
            {
#if defined(__x86_64__) || defined(_M_X64)
                respond(impl, id, "x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
                respond(impl, id, "arm64");
#else
                respond(impl, id, "unknown");
#endif
                return;
            }
            if (op == "homedir")
            {
                const char *home = std::getenv("HOME");
                respond(impl, id, home ? std::string(home) : std::string{});
                return;
            }
            if (op == "getPath")
            {
                const std::string path = req.value("path", "");
                SysPath sp = SysPath::Home;
                if (path == "userData") sp = SysPath::UserData;
                else if (path == "temp") sp = SysPath::Temp;
                else if (path == "downloads") sp = SysPath::Downloads;
                else if (path == "documents") sp = SysPath::Documents;
                else if (path == "appDir") sp = SysPath::AppDir;
                else if (path == "home") sp = SysPath::Home;
                else if (path == "cache") sp = SysPath::Cache;
                else if (path == "logs") sp = SysPath::Logs;
                else
                {
                    respond_error(impl, id, nlohmann::json{{"name", "WhizOsError"}, {"code", "OS_INVALID_ARGUMENT"}, {"message", "unknown path"}});
                    return;
                }
                respond(impl, id, impl.app->getPath(sp));
                return;
            }
            respond_error(impl, id, nlohmann::json{{"name", "WhizOsError"}, {"code", "OS_INVALID_ARGUMENT"}, {"message", "unknown op"}});
        }

        void handle_window_request(WebWindow::Impl &impl, const nlohmann::json &req)
        {
            const std::uint64_t id = req.value("id", static_cast<std::uint64_t>(0));
            const std::string op = req.value("op", "");
            const nlohmann::json args = req.value("args", nlohmann::json::array());

            const auto str_arg = [&](std::size_t i) -> std::string {
                if (args.is_array() && args.size() > i && args[i].is_string()) return args[i].get<std::string>();
                return {};
            };
            const auto int_arg = [&](std::size_t i) -> int {
                if (args.is_array() && args.size() > i && args[i].is_number()) return args[i].get<int>();
                return 0;
            };
            const auto bool_arg = [&](std::size_t i) -> bool {
                if (args.is_array() && args.size() > i && args[i].is_boolean()) return args[i].get<bool>();
                return false;
            };

            auto lock_window = [&impl]() -> saucer::window & {
                if (!impl.window) throw std::logic_error("window not realized");
                return *impl.window;
            };

            try
            {
                if (op == "setTitle") { lock_window().set_title(str_arg(0)); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "getSize") { const auto s = lock_window().size(); respond(impl, id, nlohmann::json{{"width", s.w}, {"height", s.h}}); return; }
                if (op == "setPosition") { lock_window().set_position({int_arg(0), int_arg(1)}); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "center")
                {
                    if (!impl.saucer_app) { respond(impl, id, nlohmann::json(nullptr)); return; }
                    const auto screens = impl.saucer_app->screens();
                    if (screens.empty()) { respond(impl, id, nlohmann::json(nullptr)); return; }
                    const auto &screen = screens.front();
                    const auto size = lock_window().size();
                    lock_window().set_position({screen.position.x + (screen.size.w - size.w) / 2,
                                                screen.position.y + (screen.size.h - size.h) / 2});
                    respond(impl, id, nlohmann::json(nullptr));
                    return;
                }
                if (op == "maximize") { lock_window().set_maximized(true); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "minimize") { lock_window().set_minimized(true); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "restore") { lock_window().set_maximized(false); lock_window().set_minimized(false); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "close") { lock_window().close(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "hide") { lock_window().hide(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "show") { lock_window().show(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "focus") { lock_window().focus(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "setAlwaysOnTop") { lock_window().set_always_on_top(bool_arg(0)); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "setDecorated") { lock_window().set_decorations(bool_arg(0) ? saucer::window::decoration::full : saucer::window::decoration::partial); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "openDevTools")
                {
                    // 复用 C++ 侧能力（WebWindow::openDevTools，含 permissions.devtools 校验）
                    if (impl.app)
                    {
                        if (const auto window = impl.app->getWindow(impl.id))
                        {
                            (*window)->openDevTools();
                            respond(impl, id, nlohmann::json(nullptr));
                            return;
                        }
                    }
                    respond_error(impl, id, nlohmann::json{{"name", "WhizWindowError"}, {"code", "WINDOW_INTERNAL"}, {"message", "window not found"}});
                    return;
                }
                if (op == "isMaximized") { respond(impl, id, lock_window().maximized()); return; }
                if (op == "isMinimized") { respond(impl, id, lock_window().minimized()); return; }
                if (op == "isVisible") { respond(impl, id, lock_window().visible()); return; }

                respond_error(impl, id, nlohmann::json{{"name", "WhizWindowError"}, {"code", "WINDOW_INVALID_ARGUMENT"}, {"message", "unknown op"}});
            }
            catch (const std::exception &ex)
            {
                respond_error(impl, id, nlohmann::json{{"name", "WhizWindowError"}, {"code", "WINDOW_INTERNAL"}, {"message", ex.what()}});
            }
        }
    } // namespace

    void respond(WebWindow::Impl &impl, std::uint64_t id, const nlohmann::json &result)
    {
        if (!impl.view)
        {
            return;
        }
        impl.view->execute("window.__whizResolve(" + std::to_string(id) + "," + result.dump() + ");");
    }

    void respond_error(WebWindow::Impl &impl, std::uint64_t id, const nlohmann::json &error)
    {
        if (!impl.view)
        {
            return;
        }
        impl.view->execute("window.__whizReject(" + std::to_string(id) + "," + error.dump() + ");");
    }

    void emit_to_js(WebWindow::Impl &impl, std::string_view event, const nlohmann::json &data)
    {
        if (!impl.view)
        {
            return;
        }
        const std::string code = "window.__whizEmit(" + nlohmann::json(event).dump() + "," + data.dump() + ");";
        impl.view->execute(code);
    }

    void handle_js_message(WebWindow::Impl &impl, std::string_view message)
    {
        nlohmann::json req;
        try
        {
            req = nlohmann::json::parse(message);
        }
        catch (...)
        {
            return;
        }

        const std::string channel = req.value("__whiz", "");
        if (channel == "ipc")
        {
            handle_ipc_request(impl, req);
        }
        else if (channel == "os")
        {
            handle_os_request(impl, req);
        }
        else if (channel == "window")
        {
            handle_window_request(impl, req);
        }
        else if (channel == "fs")
        {
            handle_fs_request(impl, req);
        }
        else if (channel == "dialog")
        {
            handle_dialog_request(impl, req);
        }
        else if (channel == "jserror")
        {
            if (impl.app)
            {
                impl.app->emitInternalEvent("js-error-crash", req);
            }
        }
        else if (channel == "console")
        {
            // JS console 转发到 C++ 日志（文档 11，需 permissions.log）
            const std::string level = req.value("level", "log");
            std::string line;
            if (req.contains("args") && req["args"].is_array())
            {
                for (const auto &arg : req["args"])
                {
                    if (!line.empty())
                    {
                        line += " ";
                    }
                    line += arg.is_string() ? arg.get<std::string>() : arg.dump();
                }
            }
            log_message(level, "[js] " + line);
        }
    }

    void setup_js_bridge(WebWindow::Impl &impl)
    {
        if (impl.bridge_ready)
        {
            return;
        }
        impl.bridge_ready = true;

        if (impl.view)
        {
            impl.view->inject({.code = build_js_script(impl), .run_at = saucer::script::time::creation, .clearable = false});
            impl.view->on<saucer::webview::event::message>({{.func = [impl_ptr = &impl](std::string_view message) -> saucer::status {
                if (message.find("\"__whiz\"") != std::string_view::npos)
                {
                    handle_js_message(*impl_ptr, message);
                    return saucer::status::handled;
                }
                return saucer::status::unhandled;
            }, .clearable = false}});
        }
    }
} // namespace whiz::detail

namespace whiz
{
    Subscription WebWindow::ipcHandle(std::string_view event,
                                      std::function<std::future<nlohmann::json>(const nlohmann::json &)> callback)
    {
        auto entry = std::make_shared<WebWindow::Impl::IpcHandlerEntry>();
        entry->callback = std::move(callback);

        {
            std::lock_guard<std::mutex> lock(m_impl->ipc_mutex);
            m_impl->ipc_handlers[std::string(event)].push_back(entry);
        }

        return Subscription([entry] {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->alive = false;
        });
    }

    void WebWindow::emit(std::string_view event, const nlohmann::json &data)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->view || m_impl->closed)
        {
            // 目标窗口已关闭：静默丢弃并记录警告（文档 7.1.4）
            detail::log_message("warn", "whiz: emit 到已关闭窗口，消息丢弃");
            return;
        }
        detail::emit_to_js(*m_impl, event, data);
    }
} // namespace whiz
