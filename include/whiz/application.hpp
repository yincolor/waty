#pragma once

#include <whiz/event.hpp>
#include <whiz/options.hpp>
#include <whiz/subscription.hpp>
#include <whiz/web_window.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whiz
{
    /// 应用路径（见文档 7.3.2）
    enum class SysPath
    {
        UserData,
        Temp,
        Downloads,
        Documents,
        AppDir,
        Home,
        Cache,
        Logs,
    };

    /// 应用生命周期管理（见文档 7.3）。
    class Application
    {
      public:
        explicit Application(const ApplicationOptions &options);
        ~Application();

        Application(const Application &)            = delete;
        Application &operator=(const Application &) = delete;

        /// 设置/切换应用图标
        void setIcon(std::string_view iconPath);

        /// 创建窗口
        std::shared_ptr<WebWindow> createWindow(const WebWindowOptions &options);

        /// 按 ID 获取窗口；ID 无效或对应窗口已销毁时返回 std::nullopt
        std::optional<std::shared_ptr<WebWindow>> getWindow(WindowId id) const;

        /// 枚举所有窗口
        std::vector<std::shared_ptr<WebWindow>> windows() const;

        /// 注册应用事件（生命周期、崩溃等）
        Subscription on(std::string_view event,
                        std::function<void(Event &, const nlohmann::json &)> callback);

        /// 为事件循环挂载任务（每轮事件循环触发一次，回调在 UI 线程）
        Subscription onEventLoopTick(std::function<void()> callback);

        /// 退出应用
        void quit();

        /// 运行事件循环，返回进程退出码
        int run();

        /// 应用路径
        std::string getPath(SysPath path) const;

        /// 读取嵌入资源字节；path 为相对嵌入根目录的路径（如 "icon.png"），未找到返回 std::nullopt
        std::optional<std::vector<std::uint8_t>> readEmbeddedFile(std::string_view path) const;

        // ---- 内部使用（使用者请勿调用） ----
        /// 触发应用事件（供 WebWindow / JS 桥内部使用）
        void emitInternalEvent(std::string_view event, const nlohmann::json &data);

        // ---- 单实例锁 ----
        bool requestSingleInstanceLock();
        void releaseSingleInstanceLock();
        bool hasSingleInstanceLock() const;

        // 内部实现类型（供 src/ 内部共享，使用者请勿依赖）
        struct Impl;

      private:
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
} // namespace whiz
