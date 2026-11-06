#pragma once

// 路径工具（见 DESIGN.md §7.3.2）。
//
// 两套语义：
//   * waty::path 下的同步函数 —— 纯 JSON/标准库风格的词法运算；
//   * waty::SysPath + getPath —— 按平台规则返回系统/应用目录。
//
// 字符串约定：所有输入输出均为 UTF-8；Windows 内部会转换为宽字符再运算。
// 错误约定：失败抛 PathError，其 code 与 JS 侧 WatyPathError.code 一致。
//
// 平台默认值（未设置环境变量时）：
//   Linux   : AppData=~/.local/share, Config=~/.config, Cache=~/.cache
//   Windows : AppData=%%LOCALAPPDATA%%, Config=%%APPDATA%%
//   macOS   : AppData=~/Library/Application Support, Config=~/Library/Preferences
// 应用级路径再拼 <organizationName>/<appName>；Logs 在 Config 下再加 logs。

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace waty
{
    struct ApplicationOptions;

    /// 应用路径（见文档 7.3.2）。
    enum class SysPath
    {
        AppData,      // XDG_DATA_HOME/<organizationName>/<appName>/
        Config,       // XDG_CONFIG_HOME/<organizationName>/<appName>/
        Temp,
        Downloads,
        Documents,
        Executable,   // 可执行文件完整路径（含文件名）
        Home,
        Cache,        // XDG_CACHE_HOME/<organizationName>/<appName>/
        Logs,
    };

    /// 路径操作错误。code 与 JS 侧 WatyPathError.code 保持一致。
    /// 常见 code：PATH_INVALID_ARGUMENT（参数非法，如传入了 URL）
    ///            PATH_NOT_AVAILABLE（环境变量/当前目录不可用）。
    class PathError : public std::runtime_error
    {
      public:
        PathError(std::string code, std::string message)
            : std::runtime_error(std::move(message)), m_code(std::move(code))
        {
        }

        const std::string &code() const noexcept
        {
            return m_code;
        }

      private:
        std::string m_code;
    };

    /// 同步路径工具（见文档 7.3.2）。
    ///
    /// C++ 侧使用 std::filesystem 语义，不解析符号链接，不访问文件系统
    /// （cwd / getPath 除外）。所有字符串均为 UTF-8。
    namespace path
    {
        /// 平台路径分隔符：'/' 或 '\\'
        char sep() noexcept;

        /// 平台 PATH 分隔符：':' 或 ';'
        std::string_view delimiter() noexcept;

        /// 当前工作目录（绝对路径）。
        /// 获取失败时抛出 PathError{PATH_NOT_AVAILABLE}。
        std::string cwd();

        /// 词法规范化。空输入返回 "."。
        std::string normalize(std::string_view value);

        /// 拼接路径段。使用 std::filesystem::operator/ 语义，随后 lexically_normal()。
        /// 绝对段会丢弃前面的段；空段保留分隔符；无参数或空结果返回 "."。
        std::string join(std::initializer_list<std::string_view> parts);

        template <typename... Parts>
        std::string join(Parts &&...parts)
        {
            return join(std::initializer_list<std::string_view>{std::string_view(parts)...});
        }

        /// 依次拼接各段；遇到绝对段时丢弃其左侧内容；最后相对 cwd 解析并规范化。
        std::string resolve(std::initializer_list<std::string_view> parts);

        template <typename... Parts>
        std::string resolve(Parts &&...parts)
        {
            return resolve(std::initializer_list<std::string_view>{std::string_view(parts)...});
        }

        /// 文件名（最后一段）。路径以分隔符结尾时返回 ""。
        std::string basename(std::string_view value, std::string_view ext = {});

        /// 父目录。空结果统一返回 "."。
        std::string dirname(std::string_view value);

        /// 扩展名（含点）。点文件返回 ""。
        std::string extname(std::string_view value);

        /// 是否为绝对路径（平台语义）。
        bool isAbsolute(std::string_view value);

        /// 获取系统路径。应用级路径（AppData/Config/Cache/Logs）需要 organizationName / appName，
        /// 调用本重载会抛 PATH_INVALID_ARGUMENT；其余路径（Temp/Downloads/...）可直接使用。
        std::string getPath(SysPath path);

        /// 获取系统路径，显式传入 organizationName / appName。
        std::string getPath(SysPath path, std::string_view organizationName, std::string_view appName);

        /// 从 ApplicationOptions 读取 organizationName / appName。
        /// 名称非法时抛 PATH_INVALID_ARGUMENT；不创建目录，仅返回拼接后的路径。
        std::string getPath(SysPath path, const ApplicationOptions &options);
    } // namespace path
} // namespace waty
