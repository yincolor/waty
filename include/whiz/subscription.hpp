#pragma once

#include <functional>
#include <memory>
#include <mutex>

namespace whiz
{
    /// 订阅句柄（见文档 7.2.1）。
    /// 不可拷贝，可移动；只有显式调用 unsubscribe() 才会取消订阅（析构不会自动取消）。
    /// unsubscribe 可跨线程调用。
    class Subscription
    {
        struct State
        {
            std::mutex mutex;
            std::function<void()> cancel;
            bool valid = true;
        };

      public:
        Subscription() = default;

        // 内部使用：以取消回调构造订阅句柄
        explicit Subscription(std::function<void()> cancel)
            : m_state(std::make_shared<State>())
        {
            m_state->cancel = std::move(cancel);
        }

        Subscription(Subscription &&) noexcept            = default;
        Subscription &operator=(Subscription &&) noexcept = default;

        Subscription(const Subscription &)            = delete;
        Subscription &operator=(const Subscription &) = delete;

        ~Subscription() = default;

        /// 取消订阅。取消后回调不再触发；若回调正在执行，不会中断当前回调。
        void unsubscribe()
        {
            if (!m_state)
            {
                return;
            }

            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                if (!m_state->valid)
                {
                    return;
                }
                m_state->valid = false;
                fn             = m_state->cancel;
            }

            if (fn)
            {
                fn();
            }
        }

        /// 是否仍然有效
        bool valid() const
        {
            if (!m_state)
            {
                return false;
            }

            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->valid;
        }

      private:
        std::shared_ptr<State> m_state;
    };
} // namespace whiz
