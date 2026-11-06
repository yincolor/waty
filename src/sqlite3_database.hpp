#pragma once

// 单数据库连接封装（仅 src/ 内部使用）。
//
// 每个 Sqlite3Database 独占一个 sqlite3* 与一个工作线程：
//   * 所有 SQL 通过 submit() 投递到该线程，严格按投递顺序执行；
//   * 不同数据库之间互不阻塞；
//   * 关闭时先标记 stopping（后续 submit 抛 SQLITE3_CLOSING），
//     再排空已入队任务并 join，最后 sqlite3_close_v2。
//
// 异常传播：任务体抛出的异常由 packaged_task 写入 future，不会打断工作线程。

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
        /// 模板参数 F 的签名必须为 result(sqlite3*)，result 任意可放入 future 的类型。
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
        // m_db 的所有权在本类：析构时才 close；任务内不得自行 close。
        std::string m_path;
        sqlite3 *m_db = nullptr;

        std::mutex m_mtx;
        std::condition_variable_any m_cv;
        std::deque<std::function<void(sqlite3 *)>> m_tasks;
        bool m_stopping = false;

        std::jthread m_worker;
    };
} // namespace waty::detail
