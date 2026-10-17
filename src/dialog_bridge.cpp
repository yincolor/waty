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
        GtkWindow *parent_window(WebWindow::Impl &impl)
        {
            if (!impl.window)
            {
                return nullptr;
            }
            return GTK_WINDOW(impl.window->native().window);
        }

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
        std::string str_field(const nlohmann::json &j, const char *key, std::string fallback = "")
        {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_string())
            {
                return fallback;
            }
            return it->get<std::string>();
        }

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
            // MessageBoxOptions 的字符串字段是 string_view；必须先保存到本地 std::string，
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
