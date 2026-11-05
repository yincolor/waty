#include <waty/paths.hpp>

#include <waty/options.hpp>

#include "path_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <pwd.h>
    #include <unistd.h>
#else
    #include <limits.h>
    #include <pwd.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    constexpr char k_posix_sep = '/';

    /// 识别 URL scheme（scheme 至少 2 个字符），避免把 Windows 盘符 C://foo 误判为 URL。
    bool is_url_like(std::string_view value)
    {
        if (value.empty() || !std::isalpha(static_cast<unsigned char>(value[0])))
        {
            return false;
        }

        std::size_t i = 1;
        for (; i < value.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(value[i]);
            if (value[i] == ':')
            {
                break;
            }
            if (!(std::isalnum(c) || value[i] == '+' || value[i] == '-' || value[i] == '.'))
            {
                return false;
            }
        }

        // scheme 至少 2 个字符，且后面必须是 ://
        return i >= 2 && i + 2 < value.size() && value[i] == ':' && value[i + 1] == '/' && value[i + 2] == '/';
    }

    fs::path from_utf8(std::string_view value)
    {
#if defined(_WIN32)
        if (value.empty())
        {
            return {};
        }

        const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                 static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
        {
            throw waty::PathError("PATH_INVALID_ARGUMENT", "路径不是合法的 UTF-8 字符串");
        }

        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), wide.data(), length);
        return fs::path(std::move(wide));
#else
        return fs::path(value);
#endif
    }

    std::string to_utf8(const fs::path &value)
    {
#if defined(_WIN32)
        const auto &wide = value.native();
        if (wide.empty())
        {
            return {};
        }

        const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                                 static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            throw waty::PathError("PATH_INVALID_ARGUMENT", "路径无法转换为 UTF-8");
        }

        std::string utf8(static_cast<std::size_t>(length), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                              utf8.data(), length, nullptr, nullptr);
        return utf8;
#else
        return value.string();
#endif
    }

    fs::path checked_path(std::string_view value, std::string_view what = "path")
    {
        if (is_url_like(value))
        {
            throw waty::PathError("PATH_INVALID_ARGUMENT",
                                  std::string(what) + " 必须是文件系统路径，不能是 URL: " + std::string(value));
        }
        return from_utf8(value);
    }

    std::string env_value(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string{};
    }

    std::string home_dir()
    {
#if defined(_WIN32)
        if (auto value = env_value("USERPROFILE"); !value.empty())
        {
            return value;
        }

        const auto drive = env_value("HOMEDRIVE");
        const auto rest  = env_value("HOMEPATH");
        if (!drive.empty() && !rest.empty())
        {
            return drive + rest;
        }
        return {};
#else
        if (auto value = env_value("HOME"); !value.empty())
        {
            return value;
        }

        passwd entry{};
        passwd *result = nullptr;
        std::array<char, 4096> buffer{};
        if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result) == 0 &&
            result != nullptr && result->pw_dir != nullptr)
        {
            return result->pw_dir;
        }
        return {};
#endif
    }

    std::string home_required()
    {
        auto home = home_dir();
        if (home.empty())
        {
            throw waty::PathError("PATH_NOT_AVAILABLE", "无法确定用户 Home 目录（HOME/USERPROFILE 未设置）");
        }
        return home;
    }

    std::string absolute_env(const char *name)
    {
        const auto value = env_value(name);
        if (!value.empty() && from_utf8(value).is_absolute())
        {
            return value;
        }
        return {};
    }

    std::string data_home()
    {
#if defined(_WIN32)
        if (auto value = env_value("LOCALAPPDATA"); !value.empty())
        {
            return value;
        }
        return to_utf8(from_utf8(home_required()) / "AppData" / "Local");
#elif defined(__APPLE__)
        return to_utf8(from_utf8(home_required()) / "Library" / "Application Support");
#else
        if (auto value = absolute_env("XDG_DATA_HOME"); !value.empty())
        {
            return value;
        }
        return to_utf8(from_utf8(home_required()) / ".local" / "share");
#endif
    }

    std::string config_home()
    {
#if defined(_WIN32)
        if (auto value = env_value("APPDATA"); !value.empty())
        {
            return value;
        }
        return to_utf8(from_utf8(home_required()) / "AppData" / "Roaming");
#elif defined(__APPLE__)
        return to_utf8(from_utf8(home_required()) / "Library" / "Preferences");
#else
        if (auto value = absolute_env("XDG_CONFIG_HOME"); !value.empty())
        {
            return value;
        }
        return to_utf8(from_utf8(home_required()) / ".config");
#endif
    }

    std::string cache_home()
    {
#if defined(_WIN32)
        if (auto value = env_value("LOCALAPPDATA"); !value.empty())
        {
            return value;
        }
        return to_utf8(from_utf8(home_required()) / "AppData" / "Local");
#elif defined(__APPLE__)
        return to_utf8(from_utf8(home_required()) / "Library" / "Caches");
#else
        if (auto value = absolute_env("XDG_CACHE_HOME"); !value.empty())
        {
            return value;
        }
        return to_utf8(from_utf8(home_required()) / ".cache");
#endif
    }

    std::string temp_dir()
    {
#if defined(_WIN32)
        if (auto value = env_value("TEMP"); !value.empty())
        {
            return value;
        }
        if (auto value = env_value("TMP"); !value.empty())
        {
            return value;
        }
        return to_utf8(from_utf8(home_required()) / "AppData" / "Local" / "Temp");
#else
        if (auto value = env_value("TMPDIR"); !value.empty())
        {
            return value;
        }
        return "/tmp";
#endif
    }

    std::string app_scoped_path(const std::string &base, std::string_view organization,
                                std::string_view app)
    {
        if (base.empty())
        {
            throw waty::PathError("PATH_NOT_AVAILABLE", "应用路径的基础目录不可用");
        }

        return to_utf8(from_utf8(base) / from_utf8(organization) / from_utf8(app));
    }

    void validate_app_names(std::string_view organization, std::string_view app)
    {
        if (!waty::detail::is_valid_directory_name(organization))
        {
            throw waty::PathError("PATH_INVALID_ARGUMENT", "organizationName 必须匹配 ^[A-Za-z0-9_.]+$、长度 1..64，且不得以 . 开头或结尾");
        }
        if (!waty::detail::is_valid_directory_name(app))
        {
            throw waty::PathError("PATH_INVALID_ARGUMENT", "appName 必须匹配 ^[A-Za-z0-9_.]+$、长度 1..64，且不得以 . 开头或结尾");
        }
    }

    bool is_app_scoped(waty::SysPath path)
    {
        switch (path)
        {
            case waty::SysPath::AppData:
            case waty::SysPath::Config:
            case waty::SysPath::Cache:
            case waty::SysPath::Logs: return true;
            default: return false;
        }
    }

    std::string get_path(waty::SysPath path, std::string_view organization, std::string_view app,
                         bool names_are_valid)
    {
        if (is_app_scoped(path))
        {
            if (!names_are_valid)
            {
                validate_app_names(organization, app);
            }

            switch (path)
            {
                case waty::SysPath::AppData: return app_scoped_path(data_home(), organization, app);
                case waty::SysPath::Config: return app_scoped_path(config_home(), organization, app);
                case waty::SysPath::Cache: return app_scoped_path(cache_home(), organization, app);
                case waty::SysPath::Logs:
                    return to_utf8(from_utf8(app_scoped_path(config_home(), organization, app)) / "logs");
                default: break;
            }
        }

        switch (path)
        {
            case waty::SysPath::Temp: return temp_dir();
            case waty::SysPath::Downloads: return to_utf8(from_utf8(home_required()) / "Downloads");
            case waty::SysPath::Documents: return to_utf8(from_utf8(home_required()) / "Documents");
            case waty::SysPath::Executable: {
                const auto executable = waty::detail::executable_path();
                if (executable.empty())
                {
                    throw waty::PathError("PATH_NOT_AVAILABLE", "无法获取当前可执行文件路径");
                }
                return executable;
            }
            case waty::SysPath::Home: return home_required();
            case waty::SysPath::AppData:
            case waty::SysPath::Config:
            case waty::SysPath::Cache:
            case waty::SysPath::Logs: break;
        }

        throw waty::PathError("PATH_INVALID_ARGUMENT", "未知的 SysPath");
    }
} // namespace

namespace waty::path
{
    char sep() noexcept
    {
#if defined(_WIN32)
        return '\\';
#else
        return k_posix_sep;
#endif
    }

    std::string_view delimiter() noexcept
    {
#if defined(_WIN32)
        return ";";
#else
        return ":";
#endif
    }

    std::string cwd()
    {
        std::error_code ec;
        const auto value = fs::current_path(ec);
        if (ec)
        {
            throw PathError("PATH_NOT_AVAILABLE", "无法获取当前工作目录: " + ec.message());
        }
        return to_utf8(value.lexically_normal());
    }

    std::string normalize(std::string_view value)
    {
        if (value.empty())
        {
            return ".";
        }

        auto normalized = checked_path(value).lexically_normal();
        if (normalized.empty())
        {
            return ".";
        }
        return to_utf8(normalized);
    }

    std::string join(std::initializer_list<std::string_view> parts)
    {
        fs::path result;
        for (const auto part : parts)
        {
            result /= checked_path(part, "path.join 参数");
        }

        if (result.empty())
        {
            return ".";
        }

        auto normalized = result.lexically_normal();
        if (normalized.empty())
        {
            return ".";
        }
        return to_utf8(normalized);
    }

    std::string resolve(std::initializer_list<std::string_view> parts)
    {
        fs::path result;
        bool found_absolute = false;

        for (auto it = parts.end(); it != parts.begin();)
        {
            --it;
            if (it->empty())
            {
                continue;
            }

            auto part = checked_path(*it, "path.resolve 参数");
            if (part.is_absolute())
            {
                found_absolute = true;
                if (!result.empty())
                {
                    result = part / result;
                }
                else
                {
                    result = part;
                }
                break;
            }
            if (!result.empty())
            {
                result = part / result;
            }
            else
            {
                result = part;
            }
        }

        if (result.empty())
        {
            return cwd();
        }

        if (!found_absolute && !result.is_absolute())
        {
            std::error_code ec;
            const auto current = fs::current_path(ec);
            if (ec)
            {
                throw PathError("PATH_NOT_AVAILABLE", "无法获取当前工作目录: " + ec.message());
            }
            result = current / result;
        }

        auto normalized = result.lexically_normal();
        if (normalized.filename().empty())
        {
            auto parent = normalized.parent_path();
            if (!parent.empty())
            {
                normalized = parent;
            }
        }
        return to_utf8(normalized);
    }

    std::string basename(std::string_view value, std::string_view ext)
    {
        auto name = to_utf8(checked_path(value).filename());
        if (!ext.empty() && name != ext && name.size() > ext.size() && name.ends_with(ext))
        {
            name.resize(name.size() - ext.size());
        }
        return name;
    }

    std::string dirname(std::string_view value)
    {
        const auto parent = checked_path(value).parent_path();
        if (parent.empty())
        {
            return ".";
        }
        return to_utf8(parent);
    }

    std::string extname(std::string_view value)
    {
        return to_utf8(checked_path(value).extension());
    }

    bool isAbsolute(std::string_view value)
    {
        return checked_path(value).is_absolute();
    }

    std::string getPath(SysPath path)
    {
        if (is_app_scoped(path))
        {
            throw PathError("PATH_INVALID_ARGUMENT", "应用级路径需要 organizationName 和 appName");
        }
        return get_path(path, {}, {}, true);
    }

    std::string getPath(SysPath path, std::string_view organizationName, std::string_view appName)
    {
        return get_path(path, organizationName, appName, false);
    }

    std::string getPath(SysPath path, const ApplicationOptions &options)
    {
        return get_path(path, options.organizationName, options.appName, false);
    }
} // namespace waty::path

namespace waty::detail
{
    bool is_valid_directory_name(std::string_view name)
    {
        if (name.empty() || name.size() > 64)
        {
            return false;
        }

        // 允许域名形式的 "."，但目录名不得以 "." 开头或结尾；这也排除了 "." / ".."。
        if (name.front() == '.' || name.back() == '.')
        {
            return false;
        }

        for (const unsigned char c : name)
        {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') || c == '_' || c == '.';
            if (!ok)
            {
                return false;
            }
        }

        std::string upper;
        upper.reserve(name.size());
        for (const unsigned char c : name)
        {
            upper.push_back(static_cast<char>(std::toupper(c)));
        }

        static constexpr std::array<std::string_view, 22> reserved = {
            "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
            "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
            "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

        // Windows 保留名对带扩展名的形式同样生效（例如 CON.txt），因此只比较第一个点之前的部分。
        const auto dot = upper.find('.');
        const std::string_view base(upper.data(), dot == std::string::npos ? upper.size() : dot);
        return std::find(reserved.begin(), reserved.end(), base) == reserved.end();
    }

    std::string executable_path()
    {
#if defined(__linux__)
        char buffer[PATH_MAX];
        const auto length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length > 0)
        {
            buffer[length] = '\0';
            return buffer;
        }
#elif defined(_WIN32)
        std::wstring buffer(32768, L'\0');
        const auto length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < buffer.size())
        {
            buffer.resize(length);
            return to_utf8(fs::path(buffer));
        }
#elif defined(__APPLE__)
        std::uint32_t size = 0;
        ::_NSGetExecutablePath(nullptr, &size);
        if (size > 0)
        {
            std::string buffer(size, '\0');
            if (::_NSGetExecutablePath(buffer.data(), &size) == 0)
            {
                buffer.resize(std::strlen(buffer.c_str()));
                std::error_code ec;
                const auto canonical = fs::weakly_canonical(buffer, ec);
                if (!ec)
                {
                    return to_utf8(canonical);
                }
                return buffer;
            }
        }
#endif
        return {};
    }

    std::string executable_dir()
    {
        const auto executable = executable_path();
        if (!executable.empty())
        {
            return waty::path::dirname(executable);
        }

        std::error_code ec;
        const auto current = fs::current_path(ec);
        if (ec)
        {
            return {};
        }
        return to_utf8(current);
    }
} // namespace waty::detail
