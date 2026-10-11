#pragma once

#include <whiz/dialog.hpp>
#include <whiz/event.hpp>
#include <whiz/ipc_error.hpp>
#include <whiz/options.hpp>
#include <whiz/subscription.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace saucer
{
    struct application;
}

namespace whiz
{
    class Application;

    /// 窗口 ID（从 1 开始，0 表示无效）
    using WindowId = std::uint64_t;

    /// 窗口 + WebView 聚合（见文档 7.4）。
    class WebWindow
    {
      public:
        // 内部实现类型（供 src/ 内部共享，使用者请勿依赖）
        struct Impl;

        ~WebWindow();

        WebWindow(const WebWindow &)            = delete;
        WebWindow &operator=(const WebWindow &) = delete;

        /// 窗口 ID
        WindowId id() const;

        /// 加载嵌入资源中的相对路径（见文档 5 / 7.4）。
        /// 会先规范化路径，再判断是否仍在嵌入根目录内；entry 为空时抛出 std::invalid_argument。
        void loadFile(std::string_view entry);

        /// 加载远程 URL（完全继承 Saucer 默认策略）
        void loadURL(std::string_view url);

        // ---- 窗口状态与控制（阶段 2 实现） ----
        void setFullscreen(bool fullscreen);
        bool isFullscreen() const;
        void setSize(int width, int height);
        void setMinSize(int width, int height);
        void setResizable(bool resizable);
        std::pair<int, int> getSize() const;

        void setPosition(int x, int y);
        std::pair<int, int> getPosition() const;
        void center();

        void maximize();
        void minimize();
        void restore();
        void close();
        void hide();
        void show();
        void focus();
        void setAlwaysOnTop(bool enabled);
        void setDecorated(bool enabled);
        void setWindowFrameState(bool frame);
        bool hasWindowFrame() const;
        void setTitle(std::string_view title);
        std::string title() const;
        bool isMaximized() const;
        bool isMinimized() const;
        bool isVisible() const;

        // ---- 事件 / IPC / 对话框（阶段 2 实现） ----
        Subscription windowEventHandle(std::string_view event,
                                       std::function<void(Event &, const nlohmann::json &)> callback);
        Subscription ipcHandle(std::string_view event,
                               std::function<std::future<nlohmann::json>(const nlohmann::json &)> callback);
        void emit(std::string_view event, const nlohmann::json &data);
        void openDevTools();

        void showOpenDialog(const OpenDialogOptions &opts,
                            std::function<void(std::optional<std::vector<std::string>>)> callback);
        void showSaveDialog(const SaveDialogOptions &opts,
                            std::function<void(std::optional<std::string>)> callback);
        void showMessageBox(const MessageBoxOptions &opts,
                            std::function<void(MessageBoxResult)> callback);

      private:
        std::unique_ptr<Impl> m_impl;

        // 由 Application::createWindow 调用
        WebWindow(Application *app, WindowId id, const WebWindowOptions &options);

        // 由 Application::run 在 UI 线程调用，创建原生窗口 / WebView 并应用排队操作
        void realize(saucer::application *app);

        friend class Application;
    };
} // namespace whiz
