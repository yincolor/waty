#include "../platform.hpp"

#include "sni_dbus.hpp"

#include <waty/tray.hpp>

#include <dbus/dbus.h>

#if defined(__linux__)
    #include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace waty::tray
{
    namespace
    {
        std::atomic<int> g_tray_counter{0};

        // 用 gdk-pixbuf 将图标（文件或内存编码数据）解码为 RGBA 像素
        // （SNI IconPixmap 需要像素数据）。
        std::optional<Icon::Rgba> pixbuf_to_rgba(GdkPixbuf *pixbuf)
        {
            if (!pixbuf)
            {
                return std::nullopt;
            }

            // 托盘图标通常 22~24px，缩到 24px 以减少 IconPixmap 体积。
            constexpr int k_tray_icon_size = 24;
            GdkPixbuf *scaled = nullptr;
            if (gdk_pixbuf_get_width(pixbuf) > k_tray_icon_size ||
                gdk_pixbuf_get_height(pixbuf) > k_tray_icon_size)
            {
                scaled = gdk_pixbuf_scale_simple(pixbuf, k_tray_icon_size, k_tray_icon_size,
                                                 GDK_INTERP_BILINEAR);
            }
            GdkPixbuf *source = scaled ? scaled : pixbuf;

            const int width     = gdk_pixbuf_get_width(source);
            const int height    = gdk_pixbuf_get_height(source);
            const int channels  = gdk_pixbuf_get_n_channels(source);
            const int rowstride = gdk_pixbuf_get_rowstride(source);
            const guchar *pixels = gdk_pixbuf_get_pixels(source);

            Icon::Rgba rgba;
            rgba.width  = static_cast<std::uint32_t>(width);
            rgba.height = static_cast<std::uint32_t>(height);
            rgba.pixels.resize(static_cast<std::size_t>(width) * height * 4);

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const guchar *p = pixels + y * rowstride + x * channels;
                    const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4;
                    rgba.pixels[idx + 0] = static_cast<std::byte>(p[0]);  // R
                    rgba.pixels[idx + 1] = static_cast<std::byte>(p[1]);  // G
                    rgba.pixels[idx + 2] = static_cast<std::byte>(p[2]);  // B
                    rgba.pixels[idx + 3] = (channels >= 4) ? static_cast<std::byte>(p[3]) : std::byte{0xff};  // A
                }
            }

            if (scaled)
            {
                g_object_unref(scaled);
            }
            return rgba;
        }

        std::optional<Icon::Rgba> decode_icon_file(const std::filesystem::path &path)
        {
            GError *error     = nullptr;
            GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
            if (!pixbuf)
            {
                if (error)
                {
                    g_error_free(error);
                }
                return std::nullopt;
            }

            auto rgba = pixbuf_to_rgba(pixbuf);
            g_object_unref(pixbuf);
            return rgba;
        }

        // 解码嵌入资源中的图标文件字节（PNG / JPEG / ICO 等）。
        std::optional<Icon::Rgba> decode_icon_data(std::span<const std::uint8_t> data)
        {
            if (data.empty())
            {
                return std::nullopt;
            }

            GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
            GError *error           = nullptr;
            bool ok = gdk_pixbuf_loader_write(loader, data.data(), data.size(), &error) != FALSE;
            if (ok)
            {
                ok = gdk_pixbuf_loader_close(loader, &error) != FALSE;
            }

            if (!ok)
            {
                if (error)
                {
                    g_error_free(error);
                }
                g_object_unref(loader);
                return std::nullopt;
            }

            GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
            std::optional<Icon::Rgba> rgba;
            if (pixbuf)
            {
                g_object_ref(pixbuf);
                rgba = pixbuf_to_rgba(pixbuf);
                g_object_unref(pixbuf);
            }

            g_object_unref(loader);
            return rgba;
        }

        class LinuxTrayIcon final : public TrayIcon
        {
          public:
            explicit LinuxTrayIcon(std::unique_ptr<detail::SniItem> sni)
                : sni_(std::move(sni))
            {
                sni_->on_activate           = [this] { if (click_callback_) { click_callback_(); } };
                sni_->on_secondary_activate = [this] { if (right_click_callback_) { right_click_callback_(); } };
            }

            void set_icon(const Icon &icon) override
            {
                if (icon.kind() == Icon::Kind::ThemeName)
                {
                    sni_->set_icon_name(icon.theme_name());
                }
                else if (icon.kind() == Icon::Kind::Rgba)
                {
                    sni_->set_icon_pixmap(icon.rgba().pixels, icon.rgba().width, icon.rgba().height);
                }
                else if (icon.kind() == Icon::Kind::FilePath)
                {
                    // 解码文件为像素并发送 IconPixmap；失败则回退到路径形式的 IconName。
                    if (auto rgba = decode_icon_file(icon.file_path()))
                    {
                        sni_->set_icon_pixmap(std::move(rgba->pixels), rgba->width, rgba->height);
                    }
                    else
                    {
                        sni_->set_icon_name(icon.file_path().string());
                    }
                }
                else if (icon.kind() == Icon::Kind::Data)
                {
                    // 直接解码嵌入资源中的图标文件字节并发送 IconPixmap。
                    if (auto rgba = decode_icon_data(icon.data()))
                    {
                        sni_->set_icon_pixmap(std::move(rgba->pixels), rgba->width, rgba->height);
                    }
                }
            }

            void set_tooltip(std::string_view tooltip) override
            {
                sni_->set_tooltip(std::string(tooltip), {});
            }

            void set_menu(Menu menu) override
            {
                menu_ = std::move(menu);
                sni_->set_menu(&menu_);
            }

            void show() override
            {
                sni_->set_status("Active");
            }

            void hide() override
            {
                sni_->set_status("Passive");
            }

          private:
            std::unique_ptr<detail::SniItem> sni_;
            Menu menu_;
        };
    } // namespace

    namespace detail
    {
        TrayResult<std::unique_ptr<TrayIcon>> platform_create_tray_icon()
        {
            DBusConnection *conn = shared_connection();
            if (!conn)
            {
                auto init = platform_initialize();
                if (!init)
                {
                    return std::unexpected(init.error());
                }
                conn = shared_connection();
            }

            // 托盘宿主检测（文档 4.3.3）：SNI 规范标准名为 org.kde.StatusNotifierWatcher，
            // 个别实现可能用 org.freedesktop.*，均无时返回 NoTrayHost。
            DBusError error;
            dbus_error_init(&error);
            dbus_bool_t has_watcher =
                dbus_bus_name_has_owner(conn, "org.kde.StatusNotifierWatcher", &error);
            if (!has_watcher)
            {
                has_watcher = dbus_bus_name_has_owner(conn, "org.freedesktop.StatusNotifierWatcher", &error);
            }
            dbus_error_free(&error);

            if (!has_watcher)
            {
                return std::unexpected(TrayError::NoTrayHost);
            }

            const int id = g_tray_counter.fetch_add(1);
            auto sni = std::make_unique<SniItem>(conn, "tray-" + std::to_string(id), "waty-tray");
            if (auto result = sni->register_with_watcher(); !result)
            {
                return std::unexpected(result.error());
            }

            return std::make_unique<LinuxTrayIcon>(std::move(sni));
        }
    } // namespace detail
} // namespace waty::tray
