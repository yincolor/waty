#pragma once

// waty 应用生命周期模块 —— 公共 API（见 DESIGN.md §7.3）。
//
// Application 是框架的入口对象，职责：
//   1. 校验并保存 ApplicationOptions（appName / organizationName / 权限 / 日志 / 数据目录）；
//   2. 创建并持有底层 saucer::application（WebView 运行时）；
//   3. 管理窗口集合（createWindow / getWindow / windows）；
//   4. 分发应用级事件，并提供单实例锁与事件循环任务钩子。
//
// 线程模型：
//   * 构造函数、createWindow()、run() 必须在主线程（UI 线程）调用；
//   * getWindow() / windows() 内部加锁，任意线程可调用；
//   * hasSingleInstanceLock() 只是读取一个普通 bool，建议在持有锁的线程调用；
//   * on() / onEventLoopTick() 注册的回调统一在 UI 线程执行。
//
// 支持的应用事件（on() 的事件名，未知事件会抛 std::invalid_argument）：
//   ready               —— 事件循环启动、窗口已创建原生对象后触发；
//   before-quit         —— 即将退出，可 preventDefault() 阻止；
//   will-quit           —— 已确认退出，可再次 preventDefault() 阻止；
//   quit                —— 事件循环已结束（不可阻止）；
//   activate            —— 应用被激活（平台相关）；
//   window-all-closed   —— 最后一个窗口关闭；非 macOS 下默认退出，可 preventDefault() 阻止；
//   second-instance     —— 单实例模式下第二个实例启动，payload 含 argv / cwd / windowId；
//   js-error-crash      —— 页面未捕获异常 / unhandledrejection，payload 为 JS 侧上报字段。
//
// 典型用法：
//   waty::Application app({.appName = "Demo"});
//   auto win = app.createWindow({.title = "Hello"});
//   win->loadFile("index.html");
//   win->show();
//   return app.run();

#include <waty/event.hpp>
#include <waty/options.hpp>
#include <waty/paths.hpp>
#include <waty/subscription.hpp>
#include <waty/web_window.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace waty
{
    /// 应用生命周期管理（见文档 7.3）。
    ///
    /// 构造时即完成参数校验与 Saucer 初始化；窗口的原生对象延迟到 run()
    /// 在 UI 线程上创建（见 WebWindow::realize），因此 createWindow 之后可以
    /// 在 run() 之前安全地调用 loadFile / show 等操作（内部会先排队）。
    class Application
    {
      public:
        /// 构造应用。
        /// - appName / organizationName 非法时抛 std::invalid_argument；
        /// - sqlite3.maxOpenDatabases 超出 1..1024 时抛 std::invalid_argument；
        /// - Saucer 初始化失败时抛 std::runtime_error。
        explicit Application(const ApplicationOptions &options);
        ~Application();

        Application(const Application &)            = delete;
        Application &operator=(const Application &) = delete;

        /// 设置/切换应用图标。
        /// 图标路径先查嵌入资源，再按“相对可执行文件目录”解析；找不到只记录 warning，
        /// 不影响已创建窗口（仍使用系统默认图标）。已创建的窗口会立即更新图标。
        void setIcon(std::string_view iconPath);

        /// 创建窗口并返回共享句柄。
        /// 窗口 ID 从 1 单调递增；创建时只保存配置，原生窗口在 run() 时创建。
        /// 宽/高/最小宽/最小高为负时抛 std::invalid_argument。
        std::shared_ptr<WebWindow> createWindow(const WebWindowOptions &options);

        /// 按 ID 获取窗口；ID 无效或对应窗口已销毁时返回 std::nullopt。
        /// 内部持锁遍历，任意线程可调用。
        std::optional<std::shared_ptr<WebWindow>> getWindow(WindowId id) const;

        /// 枚举所有窗口（返回快照，调用方持有 shared_ptr 保证窗口存活）。
        std::vector<std::shared_ptr<WebWindow>> windows() const;

        /// 注册应用事件（生命周期、崩溃等）。
        /// 事件名必须在白名单内，否则抛 std::invalid_argument；
        /// 返回的 Subscription 需显式 unsubscribe() 才会取消。
        Subscription on(std::string_view event,
                        std::function<void(Event &, const nlohmann::json &)> callback);

        /// 为事件循环挂载任务（每轮事件循环触发一次，回调在 UI 线程）。
        /// 任务链由第一次注册启动并持续到所有任务取消；
        /// 回调中禁止再注册/取消任务（会触发断言并 abort）。
        Subscription onEventLoopTick(std::function<void()> callback);

        /// 请求退出应用（异步：实际退出由底层事件循环完成）。
        /// 会依次触发 before-quit / will-quit，二者均可 preventDefault() 阻止。
        void quit();

        /// 运行事件循环，返回进程退出码。
        /// 阻塞直到事件循环结束；期间在 UI 线程创建窗口原生对象并触发 ready 事件。
        int run();

        /// 设置/切换文件日志。std::nullopt 关闭文件日志；空字符串也视为关闭并记录 warning。
        /// 相对路径基于可执行文件目录解析，文件以追加方式打开；切换时旧文件句柄立即关闭。
        void setLogFile(std::optional<std::string_view> path);

        /// 读取嵌入资源字节；path 为相对嵌入根目录的路径（如 "icon.png"），未找到返回 std::nullopt。
        /// 由 waty_embed 生成的资源表提供数据，可用于在 C++ 侧读取前端产物（如图标）。
        std::optional<std::vector<std::uint8_t>> readEmbeddedFile(std::string_view path) const;

        // ---- 内部使用（使用者请勿调用） ----
        /// 触发应用事件（供 WebWindow / JS 桥内部使用）
        void emitInternalEvent(std::string_view event, const nlohmann::json &data);

        // ---- 单实例锁（见 DESIGN.md §7.3.4） ----
        /// 尝试获取单实例锁。
        /// 成功返回 true，并由本进程启动监听；失败返回 false，同时把当前
        /// argv / cwd 发送给已存在的主实例（主实例触发 second-instance 事件）。
        /// Linux 使用 flock + Unix domain socket 实现；其他平台目前恒返回 true。
        bool requestSingleInstanceLock();
        /// 释放单实例锁并停止监听线程（析构时自动调用，幂等）。
        void releaseSingleInstanceLock();
        /// 当前进程是否持有单实例锁。
        bool hasSingleInstanceLock() const;

        // 内部实现类型（定义在 src/detail.hpp，供 src/ 内部共享，使用者请勿依赖）
        struct Impl;

      private:
        // PImpl：隐藏 Saucer / 平台细节，保证公共头文件依赖最小化。
        std::unique_ptr<Impl> m_impl;

        // 内部：WebWindow 关闭时回调，驱动 window-all-closed 逻辑
        void handleWindowClosed();

        // 内部：单实例监听线程收到第二实例消息时回调（会投递到 UI 线程）
        void dispatchSecondInstance(nlohmann::json data);

        // 内部：onEventLoopTick 任务链
        void start_tick_chain();
        void run_tick_loop();

        // 内部：单实例锁（Linux）
        void start_instance_listener();
        void notify_primary(const std::string &sock_path);

        friend class WebWindow;
    };
} // namespace waty
