#pragma once

// waty 配置结构集合（见 DESIGN.md §6）。
//
// 包含三层配置：
//   * ApplicationOptions —— 应用级（名称、图标、数据目录、日志、权限、sqlite3）；
//   * WebWindowOptions   —— 窗口级（尺寸、边框、父窗口、WebView 数据目录）；
//   * 辅助选项           —— 权限、文件过滤器、对话框、sqlite3。
//
// 权限模型：除 IPC 外，暴露给 JS 的能力默认全部关闭（显式授权）。
// 未启用的模块既不会注入 JS 代码，也不会响应对应的桥接消息。

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace waty
{
    class WebWindow;

    /// 文件系统权限（见文档 6.1）。
    /// read 启用 window.waty.fs 的读取/stat/exists/读流；
    /// write 启用 writeFile/写流。二者相互独立。
    struct FsPermissionOptions
    {
        bool read  = false;
        bool write = false;
    };

    /// Sqlite3 配置（是否启用由 PermissionOptions::sqlite3 决定）
    struct Sqlite3Options
    {
        // 同时打开的数据库上限（1..1024）。
        std::size_t maxOpenDatabases = 64;
    };

    /// JS 权限配置（见文档 6.2）。
    /// 默认值即“最小权限”：仅 IPC 打开；需要前端能力时逐项显式开启。
    struct PermissionOptions
    {
        bool ipc      = true;   // IPC 是基础能力，默认开启的例外
        bool os       = false;  // 是否启用 os 模块（platform / arch）
        bool path     = false;  // 是否启用 window.waty.path（路径工具 / getPath / resolve）
        bool window   = false;  // 是否启用 JS 窗口控制
        bool devtools = false;  // 是否允许打开 DevTools
        bool log      = false;  // 是否转发 JS console 到 C++ 日志
        bool dialog   = false;  // 是否启用对话框模块
        bool sqlite3  = false;  // 是否启用 window.waty.sqlite3
        bool notify   = false;  // 是否启用 window.waty.notify

        FsPermissionOptions fs;
    };

    /// 应用配置（见文档 6.3）。
    /// appName / organizationName 会直接拼进用户数据目录，因此必须通过严格校验，
    /// 否则构造函数抛 std::invalid_argument（防止 ../ 之类的路径穿越）。
    struct ApplicationOptions
    {
        std::string appName;  // 应用名称 或 项目名称（严格校验：^[A-Za-z0-9_.]+$，1-64 字符，不得以 . 开头/结尾）
        std::string organizationName = "waty";  // 组织或发布者名称；用于用户数据目录拼接；校验同 appName，支持 google.com 形式
        std::string icon;     // 图标路径；留空则使用系统默认图标

        // 日志文件路径。
        // std::nullopt（或不赋值）= 关闭文件日志；空字符串视为关闭并记录 warning。
        // 相对路径基于可执行文件目录解析；文件以追加方式打开。
        std::optional<std::string> logFile;

        // WebView 运行时数据目录。
        // 留空时默认使用 waty::path::getPath(SysPath::AppData) / "webview"。
        // Linux: $XDG_DATA_HOME/<organizationName>/<appName>/webview，默认 ~/.local/share/waty/<appName>/webview。
        // Windows: %LOCALAPPDATA%\<organizationName>\<appName>\WebView2。
        // macOS: ~/Library/Application Support/<organizationName>/<appName>/WebView。
        std::optional<std::string> webviewStoragePath;

        Sqlite3Options sqlite3;

        PermissionOptions permissions;
    };

    /// 窗口配置（见文档 6.4）。
    /// title / icon 是 string_view：创建窗口时会被拷贝进内部实现，
    /// 调用方无需保证其生命周期超过 createWindow()。
    struct WebWindowOptions
    {
        std::string_view title;      // 窗口标题
        int width      = 800;        // 窗口宽度，默认 800
        int height     = 600;        // 窗口高度，默认 600
        int minWidth   = 0;          // 窗口最小宽度，默认 0
        int minHeight  = 0;          // 窗口最小高度，默认 0
        bool resizable = true;       // 是否可调整尺寸
        bool fullscreen = false;     // 是否全屏（默认非全屏）
        bool frame      = true;      // 是否存在窗口边框（默认有边框）

        std::weak_ptr<WebWindow> parentWebWindow;  // 父窗口，使用 weak_ptr 避免生命周期问题
        std::string_view icon;                     // 图标路径；留空则继承 ApplicationOptions.icon

        // 当前窗口的 WebView 运行时数据目录。
        // 留空时继承 ApplicationOptions.webviewStoragePath；
        // 若两者都为空，则使用默认平台数据目录。
        std::optional<std::string_view> storagePath;
    };
} // namespace waty
