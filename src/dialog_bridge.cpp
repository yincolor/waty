// 原生对话框桥（见 DESIGN.md §6.5 / §8.5）。
//
// 两条入口共享同一套平台实现（show_*_dialog_native）：
//   1. C++ API：WebWindow::showOpenDialog / showSaveDialog / showMessageBox；
//   2. JS 桥： handle_dialog_request 解析 { options } 后调用同一函数。
//
// 当前仅 Linux（GTK4）有完整实现；Windows/macOS 在桥接层直接回调取消/失败。
//
// 线程：必须在 UI 线程调用（GTK 对话框需要主循环）。
// 异步：GTK 的 *_async API 在回调中响应 Promise；回调数据用 new 分配、在完成时 delete。
// 生命周期：对话框对象在完成回调中 unref；使用方持有的 WebWindow::Impl 在回调期间仍然有效
// （窗口关闭时 view 为空，respond 会自行丢弃）。

#include <waty/dialog.hpp>
#include <waty/web_window.hpp>

#include "detail.hpp"

#include <saucer/window.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#if defined(__linux__)
    #include <gtk/gtk.h>
    #include <saucer/modules/stable/webkitgtk.hpp>
#endif

namespace waty::detail
{
#if defined(__linux__)
    namespace
    {
        // 取原生 GtkWindow 作为对话框父窗口（保证模态居中/焦点正确）。
        // 窗口尚未 realize 时返回 nullptr，GTK 会退化为无父窗口对话框。
        GtkWindow *parent_window(WebWindow::Impl &impl)
        {
            if (!impl.window)
            {
                return nullptr;
            }
            return GTK_WINDOW(impl.window->native().window);
        }

        // 把 FileFilter 转换为 GtkFileFilter（每个扩展名一条 "*.ext" 模式）。
        // filters 为空时不设置，GTK 默认显示所有文件。
        void set_filters(GtkFileDialog *dialog, const std::vector<FileFilter> &filters)
        {
            if (filters.empty())
            {
                return;
            }

            auto *store = g_list_store_new(G_TYPE_OBJECT);
            for (const auto &filter : filters)
            {
                auto *gfilter = gtk_file_filter_new();
                if (!filter.name.empty())
                {
                    gtk_file_filter_set_name(gfilter, filter.name.c_str());
                }
                for (const auto &ext : filter.extensions)
                {
                    gtk_file_filter_add_pattern(gfilter, ("*." + ext).c_str());
                }
                g_list_store_append(store, gfilter);
                g_object_unref(gfilter);
            }
            gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(store));
            g_object_unref(store);
        }

        // 异步回调上下文：持有对话框指针与业务回调，完成时统一释放。
        struct OpenDialogData
        {
            GtkFileDialog *dialog = nullptr;
            bool multiple = false;
            std::function<void(std::optional<std::vector<std::string>>)> callback;
        };

        struct SaveDialogData
        {
            GtkFileDialog *dialog = nullptr;
            std::function<void(std::optional<std::string>)> callback;
        };

        struct MessageBoxData
        {
            GtkAlertDialog *dialog = nullptr;
            std::function<void(MessageBoxResult)> callback;
        };

        void finish_open_single(GtkFileDialog *dialog, GAsyncResult *result, GError **error,
                                std::optional<std::vector<std::string>> &paths)
        {
            if (auto *file = gtk_file_dialog_open_finish(dialog, result, error))
            {
                if (char *path = g_file_get_path(file); path)
                {
                    paths->push_back(path);
                    g_free(path);
                }
                g_object_unref(file);
            }
        }

        // 打开文件（单选/多选）完成回调。用户取消时 GError 被设置，
        // 这里统一把 paths 置为 nullopt 以区分“取消”与“选了空”。
        void on_open_done(GObject *source, GAsyncResult *result, gpointer user_data)
        {
            auto *data = static_cast<OpenDialogData *>(user_data);
            auto *dialog = GTK_FILE_DIALOG(source);
            GError *error = nullptr;

            std::optional<std::vector<std::string>> paths = std::vector<std::string>{};

            if (data->multiple)
            {
                if (auto *model = gtk_file_dialog_open_multiple_finish(dialog, result, &error))
                {
                    const auto count = g_list_model_get_n_items(model);
                    for (guint i = 0; i < count; ++i)
                    {
                        auto *file = G_FILE(g_list_model_get_item(model, i));
                        if (char *path = g_file_get_path(file); path)
                        {
                            paths->push_back(path);
                            g_free(path);
                        }
                        g_object_unref(file);
                    }
                    g_object_unref(model);
                }
            }
            else
            {
                finish_open_single(dialog, result, &error, paths);
            }

            if (error)
            {
                paths = std::nullopt;
                g_error_free(error);
            }

            data->callback(std::move(paths));
            g_object_unref(data->dialog);
            delete data;
        }

        // 选择目录完成回调（最多返回一个路径）。
        void on_folder_done(GObject *source, GAsyncResult *result, gpointer user_data)
        {
            auto *data = static_cast<OpenDialogData *>(user_data);
            auto *dialog = GTK_FILE_DIALOG(source);
            GError *error = nullptr;

            std::optional<std::vector<std::string>> paths = std::vector<std::string>{};
            if (auto *file = gtk_file_dialog_select_folder_finish(dialog, result, &error))
            {
                if (char *path = g_file_get_path(file); path)
                {
                    paths->push_back(path);
                    g_free(path);
                }
                g_object_unref(file);
            }
            if (error)
            {
                paths = std::nullopt;
                g_error_free(error);
            }

            data->callback(std::move(paths));
            g_object_unref(data->dialog);
            delete data;
        }

        // 保存文件完成回调；取消时 path 为 nullopt。
        void on_save_done(GObject *source, GAsyncResult *result, gpointer user_data)
        {
            auto *data = static_cast<SaveDialogData *>(user_data);
            auto *dialog = GTK_FILE_DIALOG(source);
            GError *error = nullptr;

            std::optional<std::string> path;
            if (auto *file = gtk_file_dialog_save_finish(dialog, result, &error))
            {
                if (char *raw = g_file_get_path(file); raw)
                {
                    path = raw;
                    g_free(raw);
                }
                g_object_unref(file);
            }
            if (error)
            {
                path = std::nullopt;
                g_error_free(error);
            }

            data->callback(std::move(path));
            g_object_unref(data->dialog);
            delete data;
        }

        // 消息框完成回调。返回 GTK 的 response 值；用户点标题栏 X / Esc 时
        // 通常是 GTK_RESPONSE_DELETE_EVENT（负数），前端应按 <0 表示“未选择按钮”。
        void on_message_box_done(GObject *source, GAsyncResult *result, gpointer user_data)
        {
            auto *data = static_cast<MessageBoxData *>(user_data);
            auto *dialog = GTK_ALERT_DIALOG(source);
            GError *error = nullptr;

            const int response = gtk_alert_dialog_choose_finish(dialog, result, &error);
            if (error)
            {
                g_error_free(error);
            }

            // GtkAlertDialog 不支持复选框（文档 8.5.3）
            data->callback(MessageBoxResult{.response = response, .checkboxChecked = false});
            g_object_unref(data->dialog);
            delete data;
        }
    } // namespace
#endif

    // 创建并显示原生打开对话框。三种模式互斥：
    //   directory=true        → 选目录（忽略 multiSelect）；
    //   multiSelect=true      → 多选文件；
    //   否则                  → 单选文件。
    // Linux 的 GtkFileDialog 没有 showHidden 直接开关，当前忽略该选项。
    void show_open_dialog_native(WebWindow::Impl &impl, const OpenDialogOptions &options,
                                 std::function<void(std::optional<std::vector<std::string>>)> callback)
    {
#if defined(__linux__)
        auto *dialog = gtk_file_dialog_new();
        if (!options.title.empty())
        {
            gtk_file_dialog_set_title(dialog, std::string(options.title).c_str());
        }
        if (!options.defaultPath.empty())
        {
            if (auto *file = g_file_new_for_path(std::string(options.defaultPath).c_str()))
            {
                gtk_file_dialog_set_initial_folder(dialog, file);
                g_object_unref(file);
            }
        }
        set_filters(dialog, options.filters);
        // TODO(doc-gap): showHidden 在 GtkFileDialog 中无直接开关

        auto *data = new OpenDialogData{dialog, options.multiSelect && !options.directory, std::move(callback)};
        if (options.directory)
        {
            gtk_file_dialog_select_folder(dialog, parent_window(impl), nullptr, on_folder_done, data);
        }
        else if (options.multiSelect)
        {
            gtk_file_dialog_open_multiple(dialog, parent_window(impl), nullptr, on_open_done, data);
        }
        else
        {
            gtk_file_dialog_open(dialog, parent_window(impl), nullptr, on_open_done, data);
        }
#else
        // TODO(platform): Windows/macOS 原生文件选择器
        callback(std::nullopt);
#endif
    }

    // 创建并显示原生保存对话框：title / defaultPath / defaultName / filters 均可选。
    void show_save_dialog_native(WebWindow::Impl &impl, const SaveDialogOptions &options,
                                 std::function<void(std::optional<std::string>)> callback)
    {
#if defined(__linux__)
        auto *dialog = gtk_file_dialog_new();
        if (!options.title.empty())
        {
            gtk_file_dialog_set_title(dialog, std::string(options.title).c_str());
        }
        if (!options.defaultPath.empty())
        {
            if (auto *file = g_file_new_for_path(std::string(options.defaultPath).c_str()))
            {
                gtk_file_dialog_set_initial_folder(dialog, file);
                g_object_unref(file);
            }
        }
        if (!options.defaultName.empty())
        {
            gtk_file_dialog_set_initial_name(dialog, std::string(options.defaultName).c_str());
        }
        set_filters(dialog, options.filters);

        auto *data = new SaveDialogData{dialog, std::move(callback)};
        gtk_file_dialog_save(dialog, parent_window(impl), nullptr, on_save_done, data);
#else
        callback(std::nullopt);
#endif
    }

    // 创建并显示原生消息框。
    // buttons 至少一个；defaultButtonIndex/cancelButtonIndex 已在调用方校验；
    // GTK 4.12+ 才支持默认/取消按钮与复选框（旧版忽略这些字段）。
    // 注意 button_storage 先 reserve 再取 c_str()，保证指针在调用期间稳定。
    void show_message_box_native(WebWindow::Impl &impl, const MessageBoxOptions &options,
                                 std::function<void(MessageBoxResult)> callback)
    {
#if defined(__linux__)
        const std::string message = std::string(options.message);
        auto *dialog = gtk_alert_dialog_new("%s", message.c_str());

        if (!options.detail.empty())
        {
            gtk_alert_dialog_set_detail(dialog, std::string(options.detail).c_str());
        }

        std::vector<std::string> button_storage;
        std::vector<const char *> buttons;
        button_storage.reserve(options.buttons.size());
        for (const auto &label : options.buttons)
        {
            button_storage.push_back(label);
            buttons.push_back(button_storage.back().c_str());
        }
        buttons.push_back(nullptr);
        gtk_alert_dialog_set_buttons(dialog, buttons.data());

#if GTK_CHECK_VERSION(4, 12, 0)
        if (options.defaultButtonIndex >= 0)
        {
            gtk_alert_dialog_set_default_button(dialog, options.defaultButtonIndex);
        }
        if (options.cancelButtonIndex >= 0)
        {
            gtk_alert_dialog_set_cancel_button(dialog, options.cancelButtonIndex);
        }
#endif

        gtk_alert_dialog_set_modal(dialog, options.modal ? TRUE : FALSE);

        auto *data = new MessageBoxData{dialog, std::move(callback)};
        gtk_alert_dialog_choose(dialog, parent_window(impl), nullptr, on_message_box_done, data);
#else
        // TODO(platform): Windows/macOS 原生消息框
        callback(MessageBoxResult{.response = -1, .checkboxChecked = false});
#endif
    }

    namespace
    {
        // 安全读取 JSON 字符串字段：不存在或类型不符时返回 fallback。
        std::string str_field(const nlohmann::json &j, const char *key, std::string fallback = "")
        {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_string())
            {
                return fallback;
            }
            return it->get<std::string>();
        }

        // 解析 JS 侧传入的 filters 数组；非法项静默跳过（对话框不因坏过滤器而失败）。
        std::vector<FileFilter> parse_filters(const nlohmann::json &o)
        {
            std::vector<FileFilter> filters;
            const auto it = o.find("filters");
            if (it == o.end() || !it->is_array())
            {
                return filters;
            }
            for (const auto &f : *it)
            {
                FileFilter filter;
                filter.name = str_field(f, "name");
                if (f.contains("extensions") && f["extensions"].is_array())
                {
                    for (const auto &e : f["extensions"])
                    {
                        if (e.is_string())
                        {
                            filter.extensions.push_back(e.get<std::string>());
                        }
                    }
                }
                filters.push_back(std::move(filter));
            }
            return filters;
        }
    } // namespace

    // dialog 通道入口。响应形状（JS 侧 Promise 结果）：
    //   showOpenDialog → { canceled, filePaths[] }
    //   showSaveDialog → { canceled, filePath }
    //   showMessageBox → { response, checkboxChecked }
    // 参数校验失败返回 DIALOG_INVALID_ARGUMENT。
    void handle_dialog_request(WebWindow::Impl &impl, const nlohmann::json &request)
    {
        const std::uint64_t id = request.value("id", static_cast<std::uint64_t>(0));
        const std::string op = request.value("op", "");
        WebWindow::Impl *impl_ptr = &impl;
        const auto &o = request.contains("options") ? request["options"] : nlohmann::json::object();

        if (op == "showOpenDialog")
        {
            auto ctx = std::make_shared<std::pair<std::string, std::string>>(str_field(o, "title"), str_field(o, "defaultPath"));
            OpenDialogOptions options;
            options.title = ctx->first;
            options.defaultPath = ctx->second;
            options.multiSelect = o.value("multiSelect", false);
            options.showHidden = o.value("showHidden", false);
            options.directory = o.value("directory", false);
            options.filters = parse_filters(o);

            show_open_dialog_native(impl, options, [impl_ptr, id](std::optional<std::vector<std::string>> paths) {
                if (paths)
                {
                    respond(*impl_ptr, id, nlohmann::json{{"canceled", false}, {"filePaths", *paths}});
                }
                else
                {
                    respond(*impl_ptr, id, nlohmann::json{{"canceled", true}, {"filePaths", std::vector<std::string>{}}});
                }
            });
            return;
        }

        if (op == "showSaveDialog")
        {
            auto ctx = std::make_shared<std::tuple<std::string, std::string, std::string>>(
                str_field(o, "title"), str_field(o, "defaultPath"), str_field(o, "defaultName"));
            SaveDialogOptions options;
            options.title = std::get<0>(*ctx);
            options.defaultPath = std::get<1>(*ctx);
            options.defaultName = std::get<2>(*ctx);
            options.filters = parse_filters(o);

            show_save_dialog_native(impl, options, [impl_ptr, id](std::optional<std::string> path) {
                if (path)
                {
                    respond(*impl_ptr, id, nlohmann::json{{"canceled", false}, {"filePath", *path}});
                }
                else
                {
                    respond(*impl_ptr, id, nlohmann::json{{"canceled", true}, {"filePath", ""}});
                }
            });
            return;
        }

        if (op == "showMessageBox")
        {
            if (!o.contains("buttons") || !o["buttons"].is_array() || o["buttons"].empty())
            {
                respond_error(impl, id, nlohmann::json{{"name", "WatyDialogError"}, {"code", "DIALOG_INVALID_ARGUMENT"},
                                                       {"message", "buttons 至少需要一个按钮"}});
                return;
            }

            const std::string type = str_field(o, "type", "none");
            MessageBoxType message_type = MessageBoxType::None;
            if (type == "none") message_type = MessageBoxType::None;
            else if (type == "info") message_type = MessageBoxType::Info;
            else if (type == "warning") message_type = MessageBoxType::Warning;
            else if (type == "error") message_type = MessageBoxType::Error;
            else if (type == "question") message_type = MessageBoxType::Question;
            else
            {
                respond_error(impl, id, nlohmann::json{{"name", "WatyDialogError"}, {"code", "DIALOG_INVALID_ARGUMENT"},
                                                       {"message", "未知 type 字符串: " + type}});
                return;
            }

            MessageBoxOptions options;
            // 重要：MessageBoxOptions 的字符串字段是 string_view；必须先保存到本地 std::string，
            // 否则 str_field() 返回的临时字符串会在赋值后立即销毁，导致 string_view 悬空，
            // GTK/Pango 收到非法 UTF-8（中文尤其明显）。
            const std::string title = str_field(o, "title");
            const std::string message = str_field(o, "message");
            const std::string detail = str_field(o, "detail");
            const std::string checkbox_label = str_field(o, "checkboxLabel");
            options.title = title;
            options.message = message;
            options.detail = detail;
            options.type = message_type;
            options.defaultButtonIndex = o.value("defaultButtonIndex", 0);
            options.cancelButtonIndex = o.value("cancelButtonIndex", -1);
            options.checkboxLabel = checkbox_label;
            options.checkboxChecked = o.value("checkboxChecked", false);
            options.modal = o.value("modal", true);
            for (const auto &b : o["buttons"])
            {
                if (b.is_string())
                {
                    options.buttons.push_back(b.get<std::string>());
                }
            }

            // 校验 defaultButtonIndex / cancelButtonIndex 越界（文档 6.5）
            if (options.defaultButtonIndex < 0 ||
                static_cast<std::size_t>(options.defaultButtonIndex) >= options.buttons.size())
            {
                respond_error(impl, id, nlohmann::json{{"name", "WatyDialogError"}, {"code", "DIALOG_INVALID_ARGUMENT"},
                                                       {"message", "defaultButtonIndex 越界"}});
                return;
            }
            if (options.cancelButtonIndex != -1 &&
                (options.cancelButtonIndex < 0 ||
                 static_cast<std::size_t>(options.cancelButtonIndex) >= options.buttons.size()))
            {
                respond_error(impl, id, nlohmann::json{{"name", "WatyDialogError"}, {"code", "DIALOG_INVALID_ARGUMENT"},
                                                       {"message", "cancelButtonIndex 越界"}});
                return;
            }

            show_message_box_native(impl, options, [impl_ptr, id](MessageBoxResult result) {
                respond(*impl_ptr, id, nlohmann::json{{"response", result.response}, {"checkboxChecked", result.checkboxChecked}});
            });
            return;
        }

        respond_error(impl, id, nlohmann::json{{"name", "WatyDialogError"}, {"code", "DIALOG_INVALID_ARGUMENT"},
                                               {"message", "未知 dialog 操作: " + op}});
    }
} // namespace waty::detail

namespace waty
{
    // C++ API 直接调用平台实现。注意：必须在 UI 线程调用（GTK 对话框需要主循环）。
    void WebWindow::showOpenDialog(const OpenDialogOptions &opts,
                                   std::function<void(std::optional<std::vector<std::string>>)> callback)
    {
        // 文档 7.1.4：必须在 UI 线程调用
        detail::show_open_dialog_native(*m_impl, opts, std::move(callback));
    }

    void WebWindow::showSaveDialog(const SaveDialogOptions &opts,
                                   std::function<void(std::optional<std::string>)> callback)
    {
        detail::show_save_dialog_native(*m_impl, opts, std::move(callback));
    }

    void WebWindow::showMessageBox(const MessageBoxOptions &opts,
                                   std::function<void(MessageBoxResult)> callback)
    {
        detail::show_message_box_native(*m_impl, opts, std::move(callback));
    }
} // namespace waty
