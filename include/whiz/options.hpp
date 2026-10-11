#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whiz
{
    class WebWindow;

    /// 文件系统权限（见文档 6.1）
    struct FsPermissionOptions
    {
        bool read  = false;
        bool write = false;
    };

    /// JS 权限配置（见文档 6.2）
    struct PermissionOptions
    {
        bool ipc      = true;   // IPC 是基础能力，默认开启的例外
        bool os       = false;  // 是否启用 os 模块（含 getPath）
        bool window   = false;  // 是否启用 JS 窗口控制
        bool devtools = false;  // 是否允许打开 DevTools
        bool log      = false;  // 是否转发 JS console 到 C++ 日志
        bool dialog   = false;  // 是否启用对话框模块

        FsPermissionOptions fs;
    };

    /// 应用配置（见文档 6.3）
    struct ApplicationOptions
    {
        std::string_view appName;  // 应用名称 或 项目名称
        std::string_view icon;     // 图标路径；留空则使用系统默认图标
        std::optional<std::string_view> logFile;  // 日志文件路径

        PermissionOptions permissions;
    };

    /// 窗口配置（见文档 6.4）
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
    };
} // namespace whiz
