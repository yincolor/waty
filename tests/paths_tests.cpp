// waty paths 单测（不依赖 WebView / Saucer）。
//
// 覆盖两个部分：
//   1. waty::path 的词法工具（与 std::filesystem / Node.js path 对齐）；
//   2. SysPath + getPath 的目录规则与参数校验；
//   3. waty::detail::is_valid_directory_name（appName / organizationName 校验）。
//
// 平台相关断言使用 #if 隔离：POSIX 与 Windows 对“根/绝对路径”的定义不同。
//
// 构建：cmake -B build -DWATY_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build

#include <waty/options.hpp>
#include <waty/paths.hpp>

#include "path_internal.hpp"

#include <cstdio>
#include <string>
#include <string_view>

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

    void expect_eq(std::string_view actual, std::string_view expected, const std::string &what)
    {
        if (actual != expected)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s (actual='%s', expected='%s')\n", what.c_str(),
                         std::string(actual).c_str(), std::string(expected).c_str());
        }
    }

    template <typename F>
    void expect_path_error(F &&call, std::string_view code, const std::string &what)
    {
        try
        {
            call();
        }
        catch (const waty::PathError &ex)
        {
            if (ex.code() != code)
            {
                ++g_failures;
                std::fprintf(stderr, "FAIL: %s (code=%s, expected %s, message=%s)\n", what.c_str(),
                             ex.code().c_str(), std::string(code).c_str(), ex.what());
            }
            return;
        }
        catch (const std::exception &ex)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s (unexpected exception: %s)\n", what.c_str(), ex.what());
            return;
        }

        ++g_failures;
        std::fprintf(stderr, "FAIL: %s (no exception)\n", what.c_str());
    }

    bool ends_with(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    std::string sep()
    {
        return std::string(1, waty::path::sep());
    }
} // namespace

int main()
{
    const auto s = sep();

    // 1. 基本属性
    expect(waty::path::sep() == '/' || waty::path::sep() == '\\', "sep is '/' or '\\'");
    expect(waty::path::delimiter() == ":" || waty::path::delimiter() == ";", "delimiter is ':' or ';'");
    expect(!waty::path::cwd().empty(), "cwd is non-empty");

    // 2. normalize：空输入、"." / ".."、重复分隔符、尾部分隔符
    expect_eq(waty::path::normalize(""), ".", "normalize empty -> .");
    expect_eq(waty::path::normalize("a"), "a", "normalize keeps simple name");
    expect_eq(waty::path::normalize("./a/b"), "a/b", "normalize removes leading ./");
    expect_eq(waty::path::normalize("a/./b"), "a/b", "normalize removes inner .");
    expect_eq(waty::path::normalize("a/../b"), "b", "normalize resolves ..");
    expect_eq(waty::path::normalize("a//b"), "a/b", "normalize collapses separators");
    expect_eq(waty::path::normalize("../a"), "../a", "normalize keeps leading ..");
    expect_eq(waty::path::normalize("a/b/.."), "a/", "normalize keeps trailing separator after ..");
#if !defined(_WIN32)
    expect_eq(waty::path::normalize("/a//b/"), "/a/b/", "normalize keeps absolute + trailing separator");
    expect_eq(waty::path::normalize("//a"), "/a", "normalize collapses leading //");
#endif

    // 3. join：拼接、绝对段覆盖、空段、空参数
    expect_eq(waty::path::join("a", "b"), "a" + s + "b", "join two relative parts");
    expect_eq(waty::path::join("a", ""), "a" + s, "join empty part keeps separator");
    expect_eq(waty::path::join("", "a"), "a", "join leading empty part is ignored");
    expect_eq(waty::path::join(), ".", "join no parts -> .");
    expect_eq(waty::path::join(""), ".", "join only empty part -> .");
    expect_eq(waty::path::join("a", "..", "b"), "b", "join normalizes ..");
#if !defined(_WIN32)
    expect_eq(waty::path::join("/a", "b"), "/a/b", "join absolute + relative");
    expect_eq(waty::path::join("a", "/b", "c"), "/b/c", "join absolute part discards left side");
#endif

    // 4. resolve：相对 cwd 解析、右起绝对段覆盖
    expect_eq(waty::path::resolve(), waty::path::cwd(), "resolve() -> cwd");
    expect_eq(waty::path::resolve("a"), waty::path::join(waty::path::cwd(), "a"),
              "resolve relative -> cwd/a");
#if !defined(_WIN32)
    expect_eq(waty::path::resolve("/tmp", "a"), "/tmp/a", "resolve absolute + relative");
    expect_eq(waty::path::resolve("a", "/tmp"), "/tmp", "resolve right-most absolute wins");
#endif

    // 5. basename / dirname / extname / isAbsolute
    expect_eq(waty::path::basename("/a/b.txt"), "b.txt", "basename returns file name");
    expect_eq(waty::path::basename("/a/b.txt", ".txt"), "b", "basename strips extension");
    expect_eq(waty::path::basename("/a/"), "", "basename of trailing separator is empty");
    expect_eq(waty::path::dirname("a"), ".", "dirname simple name -> .");
    expect_eq(waty::path::dirname("/a/b"), "/a", "dirname absolute path");
#if !defined(_WIN32)
    expect_eq(waty::path::dirname("/"), "/", "dirname root is root");
    expect(waty::path::isAbsolute("/"), "isAbsolute root");
    expect(waty::path::isAbsolute("/a"), "isAbsolute /a");
#endif
    expect(!waty::path::isAbsolute("a"), "relative path is not absolute");
    expect_eq(waty::path::extname("a.tar.gz"), ".gz", "extname returns last extension");
    expect_eq(waty::path::extname(".bashrc"), "", "extname of dotfile is empty");
    expect_eq(waty::path::extname("noext"), "", "extname without dot is empty");
    expect_eq(waty::path::extname("a."), ".", "extname of trailing dot is .");

    // 6. URL 不是文件系统路径：统一抛 PATH_INVALID_ARGUMENT
    expect_path_error([] { waty::path::normalize("https://example.com/a"); },
                      "PATH_INVALID_ARGUMENT", "normalize rejects URL");
    expect_path_error([] { waty::path::join("a", "http://example.com"); },
                      "PATH_INVALID_ARGUMENT", "join rejects URL");
    expect_path_error([] { waty::path::isAbsolute("ftp://example.com"); },
                      "PATH_INVALID_ARGUMENT", "isAbsolute rejects URL");

    // 7. SysPath / getPath
    expect(!waty::path::getPath(waty::SysPath::Temp).empty(), "Temp path is non-empty");
    expect_path_error([] { waty::path::getPath(waty::SysPath::AppData); },
                      "PATH_INVALID_ARGUMENT", "AppData without names is rejected");

    const auto app_data = waty::path::getPath(waty::SysPath::AppData, "org", "app");
    expect(ends_with(app_data, "org" + s + "app"), "AppData ends with <org>/<app> (" + app_data + ")");
    const auto logs = waty::path::getPath(waty::SysPath::Logs, "org", "app");
    expect(ends_with(logs, "org" + s + "app" + s + "logs"), "Logs ends with <org>/<app>/logs (" + logs + ")");
    expect_path_error([] { waty::path::getPath(waty::SysPath::Config, "../evil", "app"); },
                      "PATH_INVALID_ARGUMENT", "getPath rejects traversal organizationName");
    expect_path_error([] { waty::path::getPath(waty::SysPath::Config, "org", ""); },
                      "PATH_INVALID_ARGUMENT", "getPath rejects empty appName");

    waty::ApplicationOptions options;
    options.appName          = "demo";
    options.organizationName = "example";
    const auto config        = waty::path::getPath(waty::SysPath::Config, options);
    expect(ends_with(config, "example" + s + "demo"), "getPath(ApplicationOptions) uses both names");

    const auto executable = waty::path::getPath(waty::SysPath::Executable);
    expect(!executable.empty(), "Executable path is non-empty");
    expect(waty::path::isAbsolute(executable), "Executable path is absolute");

    // 8. 目录名校验（ApplicationOptions / PathError 共用）
    using waty::detail::is_valid_directory_name;
    expect(is_valid_directory_name("waty"), "valid simple name");
    expect(is_valid_directory_name("Movie_2"), "valid name with underscore/digit");
    expect(is_valid_directory_name("google.com"), "valid domain-like name");
    expect(is_valid_directory_name(std::string(64, 'a')), "64-char name accepted");
    expect(is_valid_directory_name("COM0"), "COM0 is not a reserved device");
    expect(!is_valid_directory_name(""), "empty name rejected");
    expect(!is_valid_directory_name(std::string(65, 'a')), "65-char name rejected");
    expect(!is_valid_directory_name(".hidden"), "leading dot rejected");
    expect(!is_valid_directory_name("trailing."), "trailing dot rejected");
    expect(!is_valid_directory_name("a b"), "space rejected");
    expect(!is_valid_directory_name("a/b"), "slash rejected");
    expect(!is_valid_directory_name("a\\b"), "backslash rejected");
    expect(!is_valid_directory_name("CON"), "Windows reserved CON rejected");
    expect(!is_valid_directory_name("con.txt"), "Windows reserved CON.txt rejected");
    expect(!is_valid_directory_name("LPT9"), "Windows reserved LPT9 rejected");

    if (g_failures == 0)
    {
        std::printf("paths tests passed\n");
    }
    else
    {
        std::fprintf(stderr, "%d paths test(s) failed\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
