// waty 通知（notify）单测：send() 的同步字段校验 + 平台辅助接口。
//
// 这里只覆盖“投递前校验”路径：校验失败时 send() 不会初始化后端、
// 不会连接 D-Bus/启动事件线程，因此测试是确定性且无副作用的。
// 真正的通知投递/回调属于集成测试范围（需要桌面会话）。
//
// 构建：cmake -B build -DWATY_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build

#include <waty/notify.hpp>

#include <chrono>
#include <climits>
#include <cstdio>
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

    void expect_send_error(waty::notify::notification notification,
                           waty::notify::errc expected,
                           const std::string &what)
    {
        auto result = waty::notify::send(std::move(notification));
        if (result.has_value())
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s (send unexpectedly succeeded)\n", what.c_str());
            return;
        }
        if (result.error().code != expected)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s (code mismatch, message=%s)\n", what.c_str(),
                         result.error().message.c_str());
            return;
        }
        if (result.error().message.empty())
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s (error message is empty)\n", what.c_str());
        }
    }

    waty::notify::action make_action(std::string id, std::string label)
    {
        return waty::notify::action{.id = std::move(id), .label = std::move(label)};
    }
} // namespace

int main()
{
    using waty::notify::errc;
    using waty::notify::notification;

    // 1. title 必填
    {
        notification n;
        expect_send_error(std::move(n), errc::invalid_argument, "empty title rejected");
    }

    // 2. actions 上限 3
    {
        notification n;
        n.title = "hello";
        n.actions = {make_action("a", "A"), make_action("b", "B"),
                     make_action("c", "C"), make_action("d", "D")};
        expect_send_error(std::move(n), errc::invalid_argument, "more than 3 actions rejected");
    }

    // 3. action id / label 非空且 id 唯一
    {
        notification n;
        n.title   = "hello";
        n.actions = {make_action("", "A")};
        expect_send_error(std::move(n), errc::invalid_argument, "empty action id rejected");
    }
    {
        notification n;
        n.title   = "hello";
        n.actions = {make_action("a", "")};
        expect_send_error(std::move(n), errc::invalid_argument, "empty action label rejected");
    }
    {
        notification n;
        n.title   = "hello";
        n.actions = {make_action("dup", "A"), make_action("dup", "B")};
        expect_send_error(std::move(n), errc::invalid_argument, "duplicate action ids rejected");
    }

    // 4. timeout 范围：(0, INT_MAX] 毫秒
    {
        notification n;
        n.title   = "hello";
        n.timeout = std::chrono::milliseconds(0);
        expect_send_error(std::move(n), errc::invalid_argument, "zero timeout rejected");
    }
    {
        notification n;
        n.title   = "hello";
        n.timeout = std::chrono::milliseconds(-1);
        expect_send_error(std::move(n), errc::invalid_argument, "negative timeout rejected");
    }
    {
        notification n;
        n.title   = "hello";
        n.timeout = std::chrono::milliseconds(static_cast<long long>(INT_MAX) + 1);
        expect_send_error(std::move(n), errc::invalid_argument, "timeout greater than INT_MAX rejected");
    }

    // 5. tag 不能是空字符串（未提供是可选的）
    {
        notification n;
        n.title = "hello";
        n.tag   = std::string{};
        expect_send_error(std::move(n), errc::invalid_argument, "empty tag rejected");
    }

    // 6. 平台辅助接口：非 Windows 下 AUMID 注册返回 not_supported
#if !defined(_WIN32)
    {
        auto result = waty::notify::platform::windows::register_aumid("com.example.waty", "waty");
        expect(!result.has_value(), "register_aumid is not supported on this platform");
        expect(result.error().code == errc::not_supported, "register_aumid -> not_supported");
        waty::notify::platform::windows::set_aumid("com.example.waty"); // 静默忽略
    }
#endif

    if (g_failures == 0)
    {
        std::printf("notify tests passed\n");
    }
    else
    {
        std::fprintf(stderr, "%d notify test(s) failed\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
