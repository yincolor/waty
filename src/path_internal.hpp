#pragma once

#include <string>
#include <string_view>

namespace waty::detail
{
    /// appName / organizationName 目录名严格校验（与路径模块共享）。
    bool is_valid_directory_name(std::string_view name);

    /// 当前可执行文件完整路径；失败时返回空字符串。
    std::string executable_path();

    /// 当前可执行文件所在目录；失败时回退到当前工作目录。
    std::string executable_dir();
} // namespace waty::detail
