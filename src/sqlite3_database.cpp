// 单数据库工作线程实现。
// 关闭流程：置 m_stopping → request_stop + notify_all → join（已入队任务先执行完）
//           → sqlite3_close_v2。sqlite3_close_v2 是“宽容关闭”：
// 即使还有未 finalize 的 statement，SQLite 也会在它们释放后真正关闭。

#include "sqlite3_database.hpp"

namespace waty::detail
{
    Sqlite3Database::Sqlite3Database(sqlite3 *db, std::string path)
        : m_path(std::move(path)), m_db(db), m_worker([this](std::stop_token stop) { run(stop); })
    {
    }

    Sqlite3Database::~Sqlite3Database()
    {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_stopping = true;
        }
        m_worker.request_stop();
        m_cv.notify_all();
        m_worker.join(); // 已入队任务会执行完再退出

        if (m_db != nullptr)
        {
            sqlite3_close_v2(m_db); // 宽容模式：残留 statement 由 SQLite 自行收尾
            m_db = nullptr;
        }
    }

    // 工作线程主循环：等待“有任务”或“已停止”；任务在锁外执行，
    // 因此提交新任务不会被长 SQL 阻塞。停止后仍会把队列排空再退出。
    void Sqlite3Database::run(std::stop_token stop)
    {
        for (;;)
        {
            std::function<void(sqlite3 *)> task;
            {
                std::unique_lock<std::mutex> lock(m_mtx);
                m_cv.wait(lock, stop, [this] { return m_stopping || !m_tasks.empty(); });

                if (m_tasks.empty())
                {
                    return; // 已停止且队列排空
                }

                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }

            task(m_db); // packaged_task 会把异常写入 future，这里不会抛出
        }
    }
} // namespace waty::detail
