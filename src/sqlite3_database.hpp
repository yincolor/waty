#pragma once

#include <sqlite3.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace waty::detail
{
    /// Sqlite3 相关失败的统一异常：携带稳定 code 与 sqlite3_errmsg 文本。
    class Sqlite3Error : public std::runtime_error
    {
      public:
        Sqlite3Error(std::string code, std::string message)
            : std::runtime_error(std::move(message)), m_code(std::move(code))
        {
        }

        const std::string &code() const noexcept { return m_code; }

      private:
        std::string m_code;
    };

    /// 单库连接：独占 sqlite3*、独立工作线程、串行任务队列。
    /// 库与库之间互不阻塞；同一连接上的 SQL 严格按投递顺序执行。
    class Sqlite3Database
    {
      public:
        Sqlite3Database(sqlite3 *db, std::string path);
        ~Sqlite3Database();

        Sqlite3Database(const Sqlite3Database &)            = delete;
        Sqlite3Database &operator=(const Sqlite3Database &) = delete;

        /// 投递任务到本库工作线程；返回的 future 由调用方等待。
        /// 关闭中会抛 Sqlite3Error{SQLITE3_CLOSING}（调用方用 guard 转为 rejected future）。
        template <typename F>
        auto submit(F &&call) -> std::future<std::invoke_result_t<F, sqlite3 *>>
        {
            using result_t = std::invoke_result_t<F, sqlite3 *>;

            auto task   = std::make_shared<std::packaged_task<result_t(sqlite3 *)>>(std::forward<F>(call));
            auto future = task->get_future();
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                if (m_stopping)
                {
                    throw Sqlite3Error{"SQLITE3_CLOSING", "database is closing"};
                }
                m_tasks.emplace_back([task](sqlite3 *db) { (*task)(db); });
            }
            m_cv.notify_one();
            return future;
        }

        const std::string &path() const noexcept { return m_path; }

      private:
        void run(std::stop_token stop);

        // 声明顺序即构造顺序：m_worker 必须最后声明，
        // 否则线程可能在 m_mtx / m_cv / m_tasks 构造完成前就进入 run()。
        std::string m_path;
        sqlite3 *m_db = nullptr;

        std::mutex m_mtx;
        std::condition_variable_any m_cv;
        std::deque<std::function<void(sqlite3 *)>> m_tasks;
        bool m_stopping = false;

        std::jthread m_worker;
    };
} // namespace waty::detail
