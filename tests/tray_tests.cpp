// waty 托盘（tray）纯数据层单测（不创建平台托盘、不连接 D-Bus）。
//
// 覆盖：
//   * Menu 的构造 / add_item / clear / items；
//   * MenuItem 的默认值与类型（Normal / Checkbox / Separator / Submenu）；
//   * MenuItem::on_click 回调；
//   * Icon 四种来源的工厂与失败路径（ThemeName / Rgba / FilePath / Data）。
//
// 注意：TrayContext::initialize() 与 TrayIcon::create() 依赖平台宿主，
// 属于集成测试范围，这里不涉及。
//
// 构建：cmake -B build -DWATY_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build

#include <waty/tray.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    int g_failures = 0;

    void expect(bool condition, const std::string &what)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
    }

    std::filesystem::path make_temp_file()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = std::filesystem::temp_directory_path() /
                    ("waty_tray_tests_" + std::to_string(stamp) + ".png");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "not-a-real-png";
        return path;
    }
} // namespace

int main()
{
    using namespace waty::tray;

    // 1. Menu 默认状态与基本操作
    {
        Menu menu;
        expect(menu.items().empty(), "default Menu is empty");

        MenuItem first;
        first.id    = "open";
        first.label = "Open";
        menu.add_item(std::move(first));
        expect(menu.items().size() == 1, "add_item appends one item");
        expect(menu.items()[0].label == "Open", "added item keeps label");
        expect(menu.items()[0].type == MenuItemType::Normal, "default MenuItem type is Normal");
        expect(menu.items()[0].enabled, "default MenuItem is enabled");
        expect(!menu.items()[0].checked, "default MenuItem is unchecked");

        menu.clear();
        expect(menu.items().empty(), "clear removes all items");
    }

    // 2. 构造时移动 items，保留顺序 / 层级 / 类型
    {
        std::vector<MenuItem> items;

        MenuItem quit;
        quit.id    = "quit";
        quit.label = "Quit";
        items.push_back(std::move(quit));

        MenuItem separator;
        separator.type = MenuItemType::Separator;
        items.push_back(std::move(separator));

        MenuItem checkbox;
        checkbox.type    = MenuItemType::Checkbox;
        checkbox.id      = "autostart";
        checkbox.label   = "Auto start";
        checkbox.checked = true;
        items.push_back(std::move(checkbox));

        MenuItem submenu;
        submenu.type  = MenuItemType::Submenu;
        submenu.id    = "more";
        submenu.label = "More";

        MenuItem child;
        child.id    = "about";
        child.label = "About";
        submenu.children.push_back(std::move(child));
        items.push_back(std::move(submenu));

        Menu menu(std::move(items));
        expect(menu.items().size() == 4, "Menu constructor keeps all items in order");
        expect(menu.items()[0].id == "quit", "item order preserved (0)");
        expect(menu.items()[1].type == MenuItemType::Separator, "separator type preserved");
        expect(menu.items()[2].checked, "checkbox checked state preserved");
        expect(menu.items()[3].children.size() == 1, "submenu children preserved");
        expect(menu.items()[3].children[0].label == "About", "submenu child label preserved");
    }

    // 3. MenuItem::on_click 可调用，并可捕获外部状态
    {
        int clicks = 0;
        MenuItem item;
        item.label    = "Click me";
        item.on_click = [&] { ++clicks; };
        expect(static_cast<bool>(item.on_click), "on_click is set");

        item.on_click();
        item.on_click();
        expect(clicks == 2, "on_click callback runs on each invocation");
    }

    // 4. Icon：主题图标名
    {
        const auto icon = Icon::from_theme_name("dialog-information");
        expect(icon.kind() == Icon::Kind::ThemeName, "from_theme_name sets Kind::ThemeName");
        expect(icon.theme_name() == "dialog-information", "from_theme_name keeps name");
    }

    // 5. Icon：内存编码数据
    {
        const std::vector<std::uint8_t> png_bytes{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        const auto icon = Icon::from_data(png_bytes);
        expect(icon.has_value(), "from_data accepts non-empty bytes");
        expect(icon->kind() == Icon::Kind::Data, "from_data sets Kind::Data");
        expect(icon->data() == png_bytes, "from_data keeps bytes");

        const auto empty = Icon::from_data({});
        expect(!empty.has_value(), "from_data rejects empty bytes");
        expect(empty.error() == TrayError::IconLoadFailed, "from_data empty -> IconLoadFailed");
    }

    // 6. Icon：RGBA 像素
    {
        std::vector<std::byte> pixels(2 * 2 * 4, std::byte{0x7F});
        const auto icon = Icon::from_rgba(pixels, 2, 2);
        expect(icon.has_value(), "from_rgba accepts 2x2 RGBA");
        expect(icon->kind() == Icon::Kind::Rgba, "from_rgba sets Kind::Rgba");
        expect(icon->rgba().width == 2 && icon->rgba().height == 2, "from_rgba keeps dimensions");
        expect(icon->rgba().pixels.size() == pixels.size(), "from_rgba keeps pixel count");

        std::vector<std::byte> too_small(2 * 2 * 4 - 1, std::byte{0});
        const auto bad_size = Icon::from_rgba(too_small, 2, 2);
        expect(!bad_size.has_value(), "from_rgba rejects wrong pixel count");
        expect(bad_size.error() == TrayError::IconLoadFailed, "from_rgba wrong size -> IconLoadFailed");

        std::vector<std::byte> one_pixel(4, std::byte{0});
        const auto zero_width = Icon::from_rgba(one_pixel, 0, 1);
        expect(!zero_width.has_value(), "from_rgba rejects zero width");
    }

    // 7. Icon：文件路径（只校验存在性，不解码）
    {
        const auto missing = Icon::from_file(std::filesystem::temp_directory_path() /
                                             "waty_tray_tests_missing_icon.png");
        expect(!missing.has_value(), "from_file rejects missing file");
        expect(missing.error() == TrayError::IconLoadFailed, "from_file missing -> IconLoadFailed");

        const auto path = make_temp_file();
        const auto icon = Icon::from_file(path);
        expect(icon.has_value(), "from_file accepts regular file");
        expect(icon->kind() == Icon::Kind::FilePath, "from_file sets Kind::FilePath");
        expect(icon->file_path() == path, "from_file keeps path");

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    if (g_failures == 0)
    {
        std::printf("tray tests passed\n");
    }
    else
    {
        std::fprintf(stderr, "%d tray test(s) failed\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
