#pragma once

// waty-tray 跨平台系统托盘库 —— 公共 API 单头文件。
//
// 本头文件是托盘库的全部公共接口，自包含。
// 命名空间：waty::tray。
//
// 实现编译进 waty 静态库；使用者仅需链接 waty::waty 并包含本头文件。

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 线程模型与所有权：
//   * TrayContext::initialize() / TrayIcon::create() 应在 UI 线程调用；
//   * on_click / on_right_click / on_double_click 与 MenuItem::on_click 均在宿主 UI 线程执行；
//   * TrayIcon::set_menu() 按值接收 Menu：菜单及其回调被拷贝进托盘对象，
//     调用方无需保证 Menu 的生命周期（但 MenuItem::icon 等资源会一同复制）。

namespace waty::tray
{
    /// 托盘库统一错误码（见 DESIGN.md §7.7）
    enum class TrayError
    {
        PlatformNotSupported,
        IconLoadFailed,
        MenuCreationFailed,
        DbusConnectionFailed,
        NativeApiFailed,
        InvalidState,
        NoTrayHost,  // 桌面环境未提供托盘宿主
        Unknown,
    };

    template <typename T>
    using TrayResult = std::expected<T, TrayError>;

    /// 托盘图标（见 DESIGN.md §7.7）。
    /// 支持四种来源：主题图标名、RGBA 像素数据、文件路径、内存中的编码图像数据。
    ///
    /// 注意：from_file() 只校验文件存在，不做解码；实际解码因平台而异：
    /// Linux 在 set_icon() 时用 gdk-pixbuf 解码并转为 SNI IconPixmap，
    /// 其它平台由各自原生 API 加载。
    class Icon
    {
      public:
        struct Rgba
        {
            std::vector<std::byte> pixels;  // RGBA，每像素 4 字节
            std::uint32_t width  = 0;
            std::uint32_t height = 0;
        };

        enum class Kind
        {
            ThemeName,
            Rgba,
            FilePath,
            Data,  // 编码后的图像文件字节（如 PNG / JPEG / ICO）
        };

        /// 从图标文件加载（当前仅校验文件存在，不解码为像素）。
        static TrayResult<Icon> from_file(const std::filesystem::path &path)
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec))
            {
                return std::unexpected(TrayError::IconLoadFailed);
            }

            Icon icon;
            icon.m_kind      = Kind::FilePath;
            icon.m_file_path = path;
            return icon;
        }

        /// 从内存中的编码图像数据创建（如 PNG / JPEG / ICO 文件字节）。
        /// 典型用法：读取 waty 的嵌入资源后直接传入。
        /// 实际解码由平台后端在 set_icon() 时完成。
        static TrayResult<Icon> from_data(std::span<const std::uint8_t> data)
        {
            if (data.empty())
            {
                return std::unexpected(TrayError::IconLoadFailed);
            }

            Icon icon;
            icon.m_kind = Kind::Data;
            icon.m_data.assign(data.begin(), data.end());
            return icon;
        }

        /// 从 RGBA 像素数据创建。
        static TrayResult<Icon> from_rgba(std::span<const std::byte> pixels,
                                          std::uint32_t width, std::uint32_t height)
        {
            if (width == 0 || height == 0 || pixels.size() != static_cast<std::size_t>(width) * height * 4)
            {
                return std::unexpected(TrayError::IconLoadFailed);
            }

            Icon icon;
            icon.m_kind = Kind::Rgba;
            icon.m_rgba.pixels.assign(pixels.begin(), pixels.end());
            icon.m_rgba.width  = width;
            icon.m_rgba.height = height;
            return icon;
        }

        /// 使用主题图标名。
        static Icon from_theme_name(std::string_view name)
        {
            Icon icon;
            icon.m_kind       = Kind::ThemeName;
            icon.m_theme_name = std::string(name);
            return icon;
        }

        [[nodiscard]] Kind kind() const noexcept
        {
            return m_kind;
        }

        [[nodiscard]] const std::string &theme_name() const noexcept
        {
            return m_theme_name;
        }

        [[nodiscard]] const Rgba &rgba() const noexcept
        {
            return m_rgba;
        }

        [[nodiscard]] const std::filesystem::path &file_path() const noexcept
        {
            return m_file_path;
        }

        [[nodiscard]] const std::vector<std::uint8_t> &data() const noexcept
        {
            return m_data;
        }

      private:
        Kind m_kind = Kind::ThemeName;
        std::string m_theme_name;
        Rgba m_rgba;
        std::filesystem::path m_file_path;
        std::vector<std::uint8_t> m_data;
    };

    /// 菜单项类型（见 DESIGN.md §7.7）
    enum class MenuItemType
    {
        Normal,
        Checkbox,
        Separator,
        Submenu,
    };

    /// 菜单项（见 DESIGN.md §7.7）。
    /// type 决定平台如何渲染：Normal 普通项、Checkbox 勾选项、Separator 分隔线、
    /// Submenu 子菜单（其 children 为子项）。id 用于事件回传（菜单点击回调会捕获该项）。
    struct MenuItem
    {
        MenuItemType type = MenuItemType::Normal;
        std::string id;
        std::string label;
        std::optional<Icon> icon;
        bool checked   = false;
        bool enabled   = true;
        std::vector<MenuItem> children;
        std::move_only_function<void()> on_click;  // 回调在宿主 UI 线程执行
    };

    /// 托盘菜单（见 DESIGN.md §7.7）。
    /// 仅是一个有序 MenuItem 容器；真正提交给平台发生在 TrayIcon::set_menu()。
    class Menu
    {
      public:
        Menu() = default;
        explicit Menu(std::vector<MenuItem> items);

        void add_item(MenuItem item);
        void clear();

        [[nodiscard]] const std::vector<MenuItem> &items() const noexcept;

      private:
        std::vector<MenuItem> m_items;
    };

    /// 托盘运行期句柄，管理平台初始化与共享状态（见 DESIGN.md §7.7）。
    /// 通常由适配器在内部持有；直接使用核心库时需自行创建并 initialize()。
    class TrayContext
    {
      public:
        TrayContext();
        ~TrayContext();

        /// 初始化平台特定环境（如 D-Bus 连接、窗口类注册）。
        TrayResult<void> initialize();

        TrayContext(const TrayContext &)            = delete;
        TrayContext &operator=(const TrayContext &) = delete;

        // 内部实现类型（供 src/ 内部共享，使用者请勿依赖）
        struct Impl;

      private:
        std::unique_ptr<Impl> m_impl;
    };

    /// 托盘图标（见 DESIGN.md §7.7）。
    /// 平台原生实现继承本类并实现纯虚方法；通过 create() 工厂创建。
    class TrayIcon
    {
      public:
        /// 创建托盘图标。无托盘宿主时返回 TrayError::NoTrayHost。
        static TrayResult<std::unique_ptr<TrayIcon>> create();

        virtual ~TrayIcon() = default;

        virtual void set_icon(const Icon &icon)             = 0;
        virtual void set_tooltip(std::string_view tooltip)  = 0;
        virtual void set_menu(Menu menu)                    = 0;
        virtual void show()                                 = 0;
        virtual void hide()                                 = 0;

        void on_click(std::move_only_function<void()> callback)
        {
            click_callback_ = std::move(callback);
        }

        void on_right_click(std::move_only_function<void()> callback)
        {
            right_click_callback_ = std::move(callback);
        }

        void on_double_click(std::move_only_function<void()> callback)
        {
            double_click_callback_ = std::move(callback);
        }

        TrayIcon(const TrayIcon &)            = delete;
        TrayIcon &operator=(const TrayIcon &) = delete;
        TrayIcon(TrayIcon &&)                 = default;
        TrayIcon &operator=(TrayIcon &&)      = default;

      protected:
        TrayIcon() = default;

        std::move_only_function<void()> click_callback_;
        std::move_only_function<void()> right_click_callback_;
        std::move_only_function<void()> double_click_callback_;
    };
} // namespace waty::tray
