#pragma once

// 路径模块内部声明（仅 src/ 使用，不对外暴露）。
// 实现见 src/paths.cpp：这些函数被 Application / WebWindow / 日志 / 图标解析共享。

#include <string>
#include <string_view>

namespace waty::detail
{
    /// appName / organizationName 目录名严格校验（与路径模块共享）。
    /// 规则：1..64 字符，仅 [A-Za-z0-9_.]，不以 '.' 开头/结尾，非 Windows 保留设备名。
    bool is_valid_directory_name(std::string_view name);

    /// 当前可执行文件完整路径；失败时返回空字符串。
    std::string executable_path();

    /// 当前可执行文件所在目录；失败时回退到当前工作目录。
    std::string executable_dir();
} // namespace waty::detail
