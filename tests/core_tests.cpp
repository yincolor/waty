// waty 核心类型单测：Event / Subscription（不依赖 WebView / Saucer）。
//
// Event 是事件回调的统一控制对象；Subscription 是所有 on()/ipcHandle()
// 返回的取消句柄。这里验证它们的语义与线程安全约束：
//   * Event::preventDefault() 置位后对所有监听器可见；
//   * Subscription 析构不会自动取消订阅；
//   * unsubscribe() 幂等、取消回调只执行一次；
//   * 移动不触发取消，移动后原对象失效；
//   * unsubscribe() 可跨线程调用。
//
// 构建：cmake -B build -DWATY_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build

#include <waty/event.hpp>
#include <waty/subscription.hpp>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

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
} // namespace

int main()
{
    // 1. Event：默认未阻止，preventDefault 幂等
    {
        waty::Event event;
        expect(!event.defaultPrevented(), "Event starts not prevented");
        event.preventDefault();
        expect(event.defaultPrevented(), "preventDefault() sets flag");
        event.preventDefault();
        expect(event.defaultPrevented(), "preventDefault() is idempotent");
    }

    // 2. 默认构造的 Subscription 无效，unsubscribe 安全
    {
        waty::Subscription empty;
        expect(!empty.valid(), "default Subscription is invalid");
        empty.unsubscribe();
        expect(!empty.valid(), "unsubscribe on empty Subscription is safe");
    }

    // 3. unsubscribe 触发取消回调且只触发一次；随后 valid() 为 false
    {
        int cancelled = 0;
        waty::Subscription subscription([&] { ++cancelled; });

        expect(subscription.valid(), "constructed Subscription is valid");
        subscription.unsubscribe();
        expect(cancelled == 1, "unsubscribe() invokes cancel callback once");
        expect(!subscription.valid(), "unsubscribed Subscription is invalid");

        subscription.unsubscribe();
        expect(cancelled == 1, "second unsubscribe() is a no-op");
    }

    // 4. 析构不会自动取消（与 RAII 不同，这是刻意设计）
    {
        int cancelled = 0;
        {
            waty::Subscription subscription([&] { ++cancelled; });
            expect(subscription.valid(), "scoped Subscription is valid");
        }
        expect(cancelled == 0, "Subscription destructor does not cancel");
    }

    // 5. 移动构造：不触发取消，源对象失效，目标对象接管取消
    {
        int cancelled = 0;
        waty::Subscription source([&] { ++cancelled; });
        waty::Subscription target(std::move(source));

        expect(!source.valid(), "moved-from Subscription is invalid");
        expect(target.valid(), "move-constructed Subscription is valid");
        expect(cancelled == 0, "move construction does not cancel");
        target.unsubscribe();
        expect(cancelled == 1, "moved Subscription can cancel");
    }

    // 6. 移动赋值：旧目标的取消回调被丢弃（不执行），目标接管新回调
    {
        int first  = 0;
        int second = 0;
        waty::Subscription target([&] { ++second; });
        waty::Subscription source([&] { ++first; });

        target = std::move(source);
        expect(second == 0, "move assignment does not cancel the previous state");
        expect(!source.valid(), "moved-from Subscription is invalid after assignment");
        expect(target.valid(), "move-assigned Subscription is valid");

        target.unsubscribe();
        expect(first == 1, "move-assigned Subscription cancels the new state");
        expect(second == 0, "previous cancel callback was never invoked");
    }

    // 7. unsubscribe 可跨线程调用
    {
        std::atomic<int> cancelled{0};
        waty::Subscription subscription([&] { cancelled.fetch_add(1); });

        std::thread worker([moved = std::move(subscription)]() mutable {
            expect(moved.valid(), "Subscription is valid inside worker thread");
            moved.unsubscribe();
        });
        worker.join();

        expect(cancelled.load() == 1, "cross-thread unsubscribe cancels once");
    }

    if (g_failures == 0)
    {
        std::printf("core tests passed\n");
    }
    else
    {
        std::fprintf(stderr, "%d core test(s) failed\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
