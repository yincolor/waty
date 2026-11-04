#include <waty/notify.hpp>

#include "detail.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace waty::detail
{
    namespace
    {
        std::string_view notify_permission_to_string(waty::notify::permission_status status)
        {
            switch (status)
            {
            case waty::notify::permission_status::not_determined: return "not_determined";
            case waty::notify::permission_status::granted:        return "granted";
            case waty::notify::permission_status::denied:         return "denied";
            case waty::notify::permission_status::provisional:    return "provisional";
            case waty::notify::permission_status::unsupported:    return "unsupported";
            }
            return "unsupported";
        }

        std::string_view notify_errc_to_code(waty::notify::errc code)
        {
            switch (code)
            {
            case waty::notify::errc::invalid_argument:    return "NOTIFY_INVALID_ARGUMENT";
            case waty::notify::errc::permission_denied:   return "NOTIFY_PERMISSION_DENIED";
            case waty::notify::errc::not_supported:       return "NOTIFY_NOT_SUPPORTED";
            case waty::notify::errc::backend_unavailable: return "NOTIFY_BACKEND_UNAVAILABLE";
            case waty::notify::errc::io_error:            return "NOTIFY_IO_ERROR";
            case waty::notify::errc::unknown:             return "NOTIFY_UNKNOWN";
            }
            return "NOTIFY_UNKNOWN";
        }

        nlohmann::json notify_error_json(const waty::notify::error &error)
        {
            return nlohmann::json{{"name", "WatyNotifyError"},
                                  {"code", notify_errc_to_code(error.code)},
                                  {"message", error.message}};
        }

        std::string string_field(const nlohmann::json &obj, const char *key, std::string fallback = {})
        {
            const auto it = obj.find(key);
            if (it == obj.end() || !it->is_string())
            {
                return fallback;
            }
            return it->get<std::string>();
        }
    } // namespace

    void handle_notify_request(WebWindow::Impl &impl, const nlohmann::json &req)
    {
        const auto id = req.value("id", static_cast<std::uint64_t>(0));

        // 防御：权限关闭时即使 JS 被注入也无法执行。
        if (!impl.permissions.notify)
        {
            respond_error(impl, id, nlohmann::json{{"name", "WatyNotifyError"},
                                                   {"code", "NOTIFY_PERMISSION_DENIED"},
                                                   {"message", "permissions.notify is disabled"}});
            return;
        }

        const std::string op = req.value("op", "");

        if (op == "send")
        {
            const nlohmann::json options = req.value("options", nlohmann::json::object());

            waty::notify::notification notification;
            notification.title = string_field(options, "title");
            notification.body  = string_field(options, "body");
            if (options.contains("icon") && options["icon"].is_string())
            {
                notification.icon = options["icon"].get<std::string>();
            }
            if (options.contains("tag") && options["tag"].is_string())
            {
                notification.tag = options["tag"].get<std::string>();
            }
            if (options.contains("timeout") && options["timeout"].is_number_integer())
            {
                notification.timeout = std::chrono::milliseconds(options["timeout"].get<long long>());
            }
            if (options.contains("actions") && options["actions"].is_array())
            {
                for (const auto &item : options["actions"])
                {
                    waty::notify::action action;
                    action.id    = string_field(item, "id");
                    action.label = string_field(item, "label");
                    notification.actions.push_back(std::move(action));
                }
            }

            // 事件回调在 notify 的事件线程触发，投递回 UI 线程后转发给 JS。
            // 持有 shared_ptr 保证窗口对象存活；窗口关闭后 emit 会自动丢弃。
            std::shared_ptr<WebWindow> window;
            if (impl.app)
            {
                if (auto found = impl.app->getWindow(impl.id))
                {
                    window = *found;
                }
            }
            auto *saucer_app = impl.saucer_app;

            notification.on_action = [window, saucer_app](std::string_view action_id) {
                const std::string id(action_id);
                post_to_ui_thread(saucer_app, [window, id] {
                    if (window) window->emit("notify-action", {{"id", id}});
                });
            };
            notification.on_closed = [window, saucer_app](waty::notify::close_reason reason) {
                post_to_ui_thread(saucer_app, [window, reason] {
                    if (window) window->emit("notify-closed", {{"reason", static_cast<int>(reason)}});
                });
            };
            notification.on_error = [window, saucer_app](const waty::notify::error &error) {
                const nlohmann::json data = notify_error_json(error);
                post_to_ui_thread(saucer_app, [window, data] {
                    if (window) window->emit("notify-error", data);
                });
            };

            if (auto result = waty::notify::send(std::move(notification)); !result)
            {
                respond_error(impl, id, notify_error_json(result.error()));
                return;
            }
            respond(impl, id, nlohmann::json{{"ok", true}});
            return;
        }

        if (op == "hasPermission")
        {
            respond(impl, id, std::string(notify_permission_to_string(waty::notify::has_permission())));
            return;
        }

        if (op == "requestPermission")
        {
            auto *impl_ptr   = &impl;
            auto *saucer_app = impl.saucer_app;
            waty::notify::request_permission([impl_ptr, saucer_app, id](waty::notify::permission_status status) {
                const std::string result(notify_permission_to_string(status));
                post_to_ui_thread(saucer_app, [impl_ptr, id, result] { respond(*impl_ptr, id, result); });
            });
            return;
        }

        respond_error(impl, id, nlohmann::json{{"name", "WatyNotifyError"},
                                               {"code", "NOTIFY_INVALID_ARGUMENT"},
                                               {"message", "unknown op: " + op}});
    }
} // namespace waty::detail
