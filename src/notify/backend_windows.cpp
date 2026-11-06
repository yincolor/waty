#include "backend.hpp"

// Windows 后端：WinRT Toast 通知。
//
// 说明：
//   * 事件线程以 STA 模式运行（winrt::init_apartment），并泵 Win32 消息。
//   * 每个通知使用唯一内部 Tag（waty-notify-<id>）；用户 tag 通过
//     tag_to_id_ 映射实现替换语义（见设计文档 §2.4.5）。
//   * 未打包应用必须先调用 platform::windows::register_aumid + set_aumid。

#include <windows.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace waty::notify::detail {
namespace {

using winrt::Windows::Data::Xml::Dom::XmlDocument;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Notifications::ToastDismissalReason;
using winrt::Windows::UI::Notifications::ToastFailedEventArgs;
using winrt::Windows::UI::Notifications::ToastNotification;
using winrt::Windows::UI::Notifications::ToastNotificationManager;
using winrt::Windows::UI::Notifications::ToastNotifier;
using winrt::Windows::UI::Notifications::ToastDismissedEventArgs;

// 库级默认 AUMID：由 platform::windows::set_aumid() 设置。
// 为空时使用 CreateToastNotifier() 的进程默认身份（打包应用/有快捷方式的场景）；
// 未打包应用必须先 register_aumid + set_aumid，否则 Toast 会被系统静默丢弃。
std::string g_default_aumid;

std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring xml_escape(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

// 生成 ToastGeneric XML。所有文本都做 XML 转义，防止标题/正文中的
// & < > " ' 破坏 XML 结构。actions 的 arguments 使用 action.id，
// 激活时通过 ToastActivatedEventArgs.Arguments() 取回。
std::wstring build_toast_xml(const notification& n) {
    std::wstring xml;
    xml += L"<toast>";
    xml += L"<visual><binding template=\"ToastGeneric\">";
    xml += L"<text>";
    xml += xml_escape(to_wide(n.title));
    xml += L"</text>";

    if (!n.body.empty()) {
        xml += L"<text>";
        xml += xml_escape(to_wide(n.body));
        xml += L"</text>";
    }

    if (n.icon) {
        // WinRT Toast 图标仅支持 file:// URI。
        xml += L"<image placement=\"appLogoOverride\" src=\"";
        xml += xml_escape(to_wide(*n.icon));
        xml += L"\" />";
    }

    xml += L"</binding></visual>";

    if (!n.actions.empty()) {
        xml += L"<actions>";
        for (const auto& a : n.actions) {
            xml += L"<action content=\"";
            xml += xml_escape(to_wide(a.label));
            xml += L"\" arguments=\"";
            xml += xml_escape(to_wide(a.id));
            xml += L"\" activationType=\"foreground\" />";
        }
        xml += L"</actions>";
    }

    xml += L"</toast>";
    return xml;
}

// WinRT 后端。所有事件回调由 WinRT 在事件线程的消息泵中触发
// （poll_once 负责 PeekMessage/DispatchMessage）。
class backend_windows final : public backend {
  public:
    backend_windows() = default;
    ~backend_windows() override {
        shutdown();
    }

    std::expected<void, error> init() override {
        // WinRT STA 公寓在事件线程首次 poll_once/send 时初始化。
        return {};
    }

    void shutdown() noexcept override {
        pending_.clear();
        tag_to_id_.clear();
    }

    permission_status permission() override {
        // Windows 无通知权限模型，初始化成功后恒定 granted（§2.4.1）。
        return permission_status::granted;
    }

    void request_permission(std::move_only_function<void(permission_status)> cb) override {
        if (cb) {
            cb(permission_status::unsupported);
        }
    }

    // 投递 Toast。流程：
    //   1. 确保事件线程已初始化 STA 公寓；
    //   2. 生成 XML，创建 ToastNotification，设置内部唯一 Tag/Group；
    //   3. 注册 Dismissed / Activated / Failed 回调；
    //   4. Show() 后再处理旧 tag 通知（先投递后替换，避免空窗期）；
    //   5. 记录 pending_，后续事件按 id 查找。
    std::expected<uint64_t, error> send(notification n) override {
        try {
            ensure_apartment();

            const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

            XmlDocument document;
            document.LoadXml(build_toast_xml(n));

            ToastNotification toast(document);
            toast.Tag(winrt::hstring(L"waty-notify-" + std::to_wstring(id)));
            toast.Group(winrt::hstring(L"waty-notify"));

            if (n.timeout) {
                using winrt::Windows::Foundation::TimeSpan;
                // TimeSpan 单位为 100ns；1ms = 10000 ticks。
                const TimeSpan span{static_cast<std::int64_t>(n.timeout->count()) * 10000};
                toast.ExpirationTime(winrt::clock::now() + span);
            }

            const auto notifier = make_notifier();
            if (!notifier) {
                return std::unexpected(error{errc::backend_unavailable,
                                             "AUMID is not registered (call platform::windows::register_aumid)"});
            }

            pending_entry entry;
            entry.callbacks = std::move(n);
            entry.toast     = toast;

            const std::uint64_t captured_id = id;
            entry.dismissed_token = toast.Dismissed(
                [this, captured_id](ToastNotification const&, ToastDismissedEventArgs const& args) {
                    on_dismissed(captured_id, args.Reason());
                });
            entry.activated_token = toast.Activated(
                [this, captured_id](ToastNotification const&, IInspectable const& args) {
                    on_activated(captured_id, args);
                });
            entry.failed_token = toast.Failed(
                [this, captured_id](ToastNotification const&, ToastFailedEventArgs const& args) {
                    on_failed(captured_id, args);
                });

            notifier.Show(toast);

            // 替换语义（§2.4.5）：先投递新通知；成功后处理旧通知。
            if (entry.callbacks.tag) {
                const auto it = tag_to_id_.find(*entry.callbacks.tag);
                if (it != tag_to_id_.end()) {
                    const std::uint64_t old_id = it->second;
                    auto old_it                = pending_.find(old_id);
                    if (old_it != pending_.end()) {
                        hide_notification(old_it->second);
                        notification old_callbacks = std::move(old_it->second.callbacks);
                        pending_.erase(old_it);
                        if (old_callbacks.on_closed) {
                            try {
                                old_callbacks.on_closed(close_reason::replaced);
                            } catch (...) {
                                std::fprintf(stderr, "waty: error: on_closed callback threw an exception\n");
                            }
                        }
                    }
                }
                tag_to_id_[*entry.callbacks.tag] = id;
            }

            pending_[id] = std::move(entry);
            return id;
        } catch (const winrt::hresult_error& e) {
            return std::unexpected(error{errc::io_error, winrt::to_string(e.message())});
        } catch (...) {
            return std::unexpected(error{errc::unknown, "WinRT toast send failed"});
        }
    }

    void poll_once() override {
        try {
            ensure_apartment();
        } catch (...) {
            return;
        }
        pump_messages();
    }

  private:
    struct pending_entry {
        notification callbacks;
        ToastNotification toast{nullptr};
        winrt::event_token dismissed_token{};
        winrt::event_token activated_token{};
        winrt::event_token failed_token{};
    };

    // 惰性初始化 WinRT STA。使用静态原子标志而非 once_flag，
    // 使重复调用开销极低；init_apartment 必须在事件线程首次使用 WinRT 前调用。
    static void ensure_apartment() {
        static std::atomic<bool> initialized{false};
        if (initialized.load(std::memory_order_acquire)) {
            return;
        }
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        initialized.store(true, std::memory_order_release);
    }

    static ToastNotifier make_notifier() {
        if (g_default_aumid.empty()) {
            return ToastNotificationManager::CreateToastNotifier();
        }
        return ToastNotificationManager::CreateToastNotifier(winrt::hstring(to_wide(g_default_aumid)));
    }

    void hide_notification(const pending_entry& entry) {
        if (!entry.toast) {
            return;
        }
        try {
            make_notifier().Hide(entry.toast);
        } catch (...) {
        }
    }

    static void pump_messages() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // 用户关闭/超时：移除 pending 并触发 on_closed。
    // TimedOut → expired，UserCanceled → dismissed，其它 → unknown。
    void on_dismissed(std::uint64_t id, ToastDismissalReason reason) {
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }

        pending_entry entry = std::move(it->second);
        pending_.erase(it);
        erase_tag_mapping(id);

        if (!entry.callbacks.on_closed) {
            return;
        }

        close_reason cr = close_reason::unknown;
        switch (reason) {
        case ToastDismissalReason::TimedOut:
            cr = close_reason::expired;
            break;
        case ToastDismissalReason::UserCanceled:
            cr = close_reason::dismissed;
            break;
        default:
            cr = close_reason::unknown;
            break;
        }

        try {
            entry.callbacks.on_closed(cr);
        } catch (...) {
            std::fprintf(stderr, "waty: error: on_closed callback threw an exception\n");
        }
    }

    // 用户点击通知正文或按钮：Arguments() 即 action.id（正文点击通常为空串）。
    // close_on_action=true 时先隐藏 Toast 并摘除 pending，保证只回调一次。
    void on_activated(std::uint64_t id, IInspectable const& args) {
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }

        std::string action_id =
            winrt::to_string(args.as<winrt::Windows::UI::Notifications::ToastActivatedEventArgs>().Arguments());

        if (!it->second.callbacks.close_on_action) {
            if (!it->second.callbacks.on_action) {
                return;
            }
            try {
                it->second.callbacks.on_action(action_id);
            } catch (...) {
                std::fprintf(stderr, "waty: error: on_action callback threw an exception\n");
            }
            return;
        }

        // 一次性 action：先摘除 pending，再隐藏 Toast；后续 Activated 会被忽略。
        pending_entry entry = std::move(it->second);
        pending_.erase(it);
        erase_tag_mapping(id);
        hide_notification(entry);

        if (entry.callbacks.on_action) {
            try {
                entry.callbacks.on_action(action_id);
            } catch (...) {
                std::fprintf(stderr, "waty: error: on_action callback threw an exception\n");
            }
        }
        if (entry.callbacks.on_closed) {
            try {
                entry.callbacks.on_closed(close_reason::action);
            } catch (...) {
                std::fprintf(stderr, "waty: error: on_closed callback threw an exception\n");
            }
        }
    }

    void erase_tag_mapping(std::uint64_t id) {
        for (auto it = tag_to_id_.begin(); it != tag_to_id_.end();) {
            if (it->second == id) {
                it = tag_to_id_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void on_failed(std::uint64_t id, ToastFailedEventArgs const& args) {
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }

        const error e{errc::io_error, winrt::to_string(args.ErrorCode().message())};
        if (it->second.callbacks.on_error) {
            try {
                it->second.callbacks.on_error(e);
            } catch (...) {
                std::fprintf(stderr, "waty: error: on_error callback threw an exception\n");
            }
        } else {
            report_error(e);
        }

        pending_.erase(it);
        erase_tag_mapping(id);
    }

    std::atomic<std::uint64_t> next_id_{0};
    std::unordered_map<std::string, std::uint64_t> tag_to_id_;
    std::unordered_map<std::uint64_t, pending_entry> pending_;
};

} // namespace

// 在 HKCU\Software\Classes\AppUserModelId\<aumid> 写入 DisplayName / IconUri。
// 未打包应用调用一次即可；重复调用是幂等的（覆盖写）。
std::expected<void, error> windows_register_aumid(std::string_view aumid,
                                                  std::string_view display_name,
                                                  std::string_view icon_path) {
    const std::wstring key = L"Software\\Classes\\AppUserModelId\\" + to_wide(aumid);

    HKEY key_handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key_handle, nullptr) != ERROR_SUCCESS) {
        return std::unexpected(error{errc::io_error, "failed to create AUMID registry key"});
    }

    const std::wstring display_name_wide = to_wide(display_name);
    const LSTATUS dn_result =
        RegSetValueExW(key_handle, L"DisplayName", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(display_name_wide.c_str()),
                       static_cast<DWORD>((display_name_wide.size() + 1) * sizeof(wchar_t)));

    LSTATUS icon_result = ERROR_SUCCESS;
    if (!icon_path.empty()) {
        const std::wstring icon_wide = to_wide(icon_path);
        icon_result =
            RegSetValueExW(key_handle, L"IconUri", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(icon_wide.c_str()),
                           static_cast<DWORD>((icon_wide.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(key_handle);

    if (dn_result != ERROR_SUCCESS || icon_result != ERROR_SUCCESS) {
        return std::unexpected(error{errc::io_error, "failed to write AUMID registry values"});
    }
    return {};
}

void windows_set_aumid(std::string_view aumid) {
    g_default_aumid = std::string(aumid);
}

std::unique_ptr<backend> make_backend() {
    return std::make_unique<backend_windows>();
}

} // namespace waty::notify::detail
