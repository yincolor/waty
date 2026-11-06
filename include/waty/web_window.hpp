#pragma once

// waty 窗口 + WebView 模块 —— 公共 API（见 DESIGN.md §7.4）。
//
// WebWindow 聚合了三件事：
//   1. 原生窗口（标题、尺寸、位置、全屏/最大化/最小化、边框、置顶）；
//   2. WebView 内容（embedded:// 嵌入资源或远程 URL）；
//   3. 与前端交互的桥接能力（ipc / fs / dialog / window / sqlite3 / notify）。
//
// 生命周期：由 Application::createWindow 构造；在 Application::run() 的 UI 线程上
// 调用 realize() 创建原生对象。realize() 之前调用窗口操作是安全的——
// 能缓存的配置会缓存，能排队的操作（loadFile / loadURL / show）会排队。
//
// 线程模型：
//   * 所有方法内部对 m_impl->mutex 加锁，可从任意线程调用；
//   * ipcHandle 的用户回调在线程池执行（不得直接操作 UI）；
//   * windowEventHandle / on() 的回调在 UI 线程执行。

#include <waty/dialog.hpp>
#include <waty/event.hpp>
#include <waty/ipc_error.hpp>
#include <waty/options.hpp>
#include <waty/subscription.hpp>

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

namespace waty
{
    class Application;

    /// 窗口 ID（从 1 开始，0 表示无效）
    using WindowId = std::uint64_t;

    /// 窗口 + WebView 聚合（见文档 7.4）。
    class WebWindow
    {
      public:
        // 内部实现类型（定义在 src/detail.hpp，供 src/ 内部共享，使用者请勿依赖）
        struct Impl;

        /// 析构时注销本窗口的 CWD 轮询任务与 SQLite 句柄（后者可能排空队列，较重）。
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
        // 说明：realize() 之前调用时：
        //   * 可缓存的属性（标题/尺寸参数/边框/全屏/可缩放）写入 Impl，创建时统一应用；
        //   * loadFile / loadURL / show 会排队，realize 后立即执行；
        //   * close / hide / openDevTools 为 no-op；
        //   * setSize / setMinSize / setPosition / center / maximize / minimize /
        //     restore / focus / setAlwaysOnTop 会抛 std::logic_error。
        // 查询类接口（isXxx / getSize / getPosition）读取缓存值，不访问原生窗口。

        /// 设置全屏状态；可在 realize() 前调用（创建时应用）。
        void setFullscreen(bool fullscreen);
        /// 返回缓存的全屏状态。
        bool isFullscreen() const;
        /// 设置窗口内容尺寸（像素）；未 realize 时抛 std::logic_error。
        void setSize(int width, int height);
        /// 设置窗口最小尺寸；未 realize 时抛 std::logic_error。
        void setMinSize(int width, int height);
        /// 设置是否允许用户调整窗口尺寸；可在 realize() 前调用。
        void setResizable(bool resizable);
        /// 返回缓存的窗口尺寸（由构造参数与 resize 事件维护）。
        std::pair<int, int> getSize() const;

        /// 设置窗口左上角屏幕坐标；未 realize 时抛 std::logic_error。会触发 move 事件。
        void setPosition(int x, int y);
        /// 返回缓存的窗口位置。
        std::pair<int, int> getPosition() const;
        /// 在首个屏幕的工作区居中；未 realize 时抛 std::logic_error。
        void center();

        /// 最大化（会触发 maximize 事件）。
        void maximize();
        /// 最小化（会触发 minimize 事件）。
        void minimize();
        /// 从最大化/最小化恢复（会触发 restore 事件）。
        void restore();
        /// 请求关闭窗口。先触发 close 事件（可 preventDefault 阻止），
        /// 真正关闭后触发 closed，并通知父窗口 sub-window-close。
        void close();
        /// 隐藏窗口并触发 hide 事件。
        void hide();
        /// 显示窗口并触发 show 事件；realize() 前调用会排队到创建后执行。
        void show();
        /// 聚焦窗口；未 realize 时抛 std::logic_error。
        void focus();
        /// 设置窗口是否始终置顶；未 realize 时抛 std::logic_error。
        void setAlwaysOnTop(bool enabled);
        /// 兼容旧接口：等价于 setWindowFrameState(enabled)。
        void setDecorated(bool enabled);
        /// 设置是否显示窗口边框/标题栏。
        /// frame=false 时使用 decoration::partial（去边框但保留阴影与缩放能力）。
        void setWindowFrameState(bool frame);
        /// 返回缓存的边框状态。
        bool hasWindowFrame() const;
        /// 设置窗口标题；realize() 前调用会在创建时应用。
        void setTitle(std::string_view title);
        /// 返回缓存的窗口标题。
        std::string title() const;
        /// 返回缓存的最大化状态（由 maximize/restore 事件维护）。
        bool isMaximized() const;
        /// 返回缓存的最小化状态（由 minimize/restore 事件维护）。
        bool isMinimized() const;
        /// 返回缓存的可见状态。
        bool isVisible() const;

        // ---- 事件 / IPC / 对话框（阶段 2 实现） ----

        /// 注册窗口事件。支持：close, closed, resize, move, focus, blur,
        /// minimize, maximize, restore, show, hide, sub-window-close；其它事件抛 std::invalid_argument。
        /// 回调在 UI 线程执行；Event::preventDefault() 目前对 close 生效。
        Subscription windowEventHandle(std::string_view event,
                                       std::function<void(Event &, const nlohmann::json &)> callback);
        // IPC 请求处理。回调在线程池执行，返回 future 表示异步响应。
        // 每个事件名只能注册一个 handler；重复注册会抛出 std::logic_error。
        // 回调抛出的 std::invalid_argument 映射为 IPC_INVALID_ARGUMENT，
        // 其它异常映射为 IPC_HANDLER_EXCEPTION，最终 reject 前端 Promise。
        Subscription ipcHandle(std::string_view event,
                               std::function<std::future<nlohmann::json>(const nlohmann::json &)> callback);
        /// 向页面推送事件：等价于 JS 侧 window.waty.ipc.on(event, cb) 收到的数据。
        /// 窗口已关闭时静默丢弃并记录 warning。
        void emit(std::string_view event, const nlohmann::json &data);
        /// 打开 DevTools；未启用 permissions.devtools 时抛 std::runtime_error。
        void openDevTools();

        /// 显示原生“打开文件/目录”对话框。
        /// 回调参数：用户选择返回路径列表；取消返回 std::nullopt。回调在 UI 线程执行。
        void showOpenDialog(const OpenDialogOptions &opts,
                            std::function<void(std::optional<std::vector<std::string>>)> callback);
        /// 显示原生“保存文件”对话框。
        /// 回调参数：确认返回目标路径；取消返回 std::nullopt。
        void showSaveDialog(const SaveDialogOptions &opts,
                            std::function<void(std::optional<std::string>)> callback);
        /// 显示原生消息框。回调参数为按钮索引与复选框状态；标题栏关闭时 response = -1。
        void showMessageBox(const MessageBoxOptions &opts,
                            std::function<void(MessageBoxResult)> callback);

      private:
        // PImpl：隐藏 Saucer 与平台原生句柄。
        std::unique_ptr<Impl> m_impl;

        // 由 Application::createWindow 调用
        WebWindow(Application *app, WindowId id, const WebWindowOptions &options);

        // 由 Application::run 在 UI 线程调用，创建原生窗口 / WebView 并应用排队操作
        void realize(saucer::application *app);

        friend class Application;
    };
} // namespace waty
