// SQLite3 服务实现（见 sqlite3_service.hpp 的整体说明）。
//
// 关键设计：
//   * open 在调用线程同步完成（用 m_open_mtx 串行化），幂等且无需“正在打开”状态机；
//   * execute/query/transaction 先 find() 提前失败，再投递到对应库的工作线程；
//   * guard() 把同步异常（如 SQLITE3_NOT_OPEN / SQLITE3_CLOSING）转为 rejected future；
//   * close/release_all 只在锁内摘除路由，真正的析构（排空队列 + join）在锁外发生。

#include "sqlite3_service.hpp"

#include "sqlite3_statement.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <system_error>
#include <vector>

namespace waty::detail
{
    namespace
    {
        using json = nlohmann::json;

        std::string to_lower(std::string_view input)
        {
            std::string out(input);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        /// 跨平台安全的 UTF-8 路径（Windows 下 path::string() 可能是 ANSI 编码）。
        /// sqlite3_open_v2 在 Windows 上按 UTF-8 解释文件名，因此统一走 u8string()。
        std::string utf8_path(const std::filesystem::path &path)
        {
            const auto u8 = path.u8string();
            return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
        }

        // 立即就绪的 shared_future（用于同步返回结果）。
        template <typename T>
        std::shared_future<T> ready(T value)
        {
            std::promise<T> promise;
            promise.set_value(std::move(value));
            return promise.get_future().share();
        }

        // 立即失败的 shared_future（用于参数校验失败）。
        template <typename T>
        std::shared_future<T> failed(std::string code, std::string message)
        {
            std::promise<T> promise;
            promise.set_exception(std::make_exception_ptr(Sqlite3Error{std::move(code), std::move(message)}));
            return promise.get_future().share();
        }

        /// 统一异常出口：同步异常 → rejected future，保证 Service 的“绝不同步抛”契约。
        template <typename T, typename F>
        std::shared_future<T> guard(F &&call) noexcept
        {
            try
            {
                return std::forward<F>(call)().share();
            }
            catch (...)
            {
                std::promise<T> promise;
                promise.set_exception(std::current_exception());
                return promise.get_future().share();
            }
        }

        // SQLite 64 位整数 → JSON。超出 JS 安全整数范围时转字符串，避免前端精度丢失。
        json int64_to_json(std::int64_t value)
        {
            if (value > k_max_safe_integer || value < -k_max_safe_integer)
            {
                return std::to_string(value);
            }
            return value;
        }

        // 执行无参数、无结果的 SQL（BEGIN/COMMIT/ROLLBACK 等）。
        // 失败抛 Sqlite3Error，错误信息优先用 sqlite3_exec 的 error 文本。
        void exec_raw(sqlite3 *db, const char *sql)
        {
            char *error  = nullptr;
            const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
            if (rc != SQLITE_OK)
            {
                const std::string message = error != nullptr ? error : sqlite3_errmsg(db);
                sqlite3_free(error);
                throw Sqlite3Error{"SQLITE3_EXEC_ERROR", message};
            }
        }
    } // namespace

    Sqlite3Service::Sqlite3Service(std::filesystem::path data_dir, std::size_t max_open)
        : m_data_dir(std::move(data_dir)), m_max_open(max_open)
    {
    }

    bool Sqlite3Service::valid_db_name(std::string_view name)
    {
        if (name.empty() || name.size() > 64)
        {
            return false;
        }
        return std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_';
        });
    }

    // 版本格式：^\d{1,3}\.\d{1,3}\.\d{1,3}$（手工扫描，避免正则依赖）。
    bool Sqlite3Service::valid_version(std::string_view version)
    {
        // ^\d{1,3}\.\d{1,3}\.\d{1,3}$
        int part           = 0;
        std::size_t digits = 0;
        for (const char c : version)
        {
            if (c >= '0' && c <= '9')
            {
                if (++digits > 3)
                {
                    return false;
                }
                continue;
            }
            if (c == '.' && digits > 0 && part < 2)
            {
                ++part;
                digits = 0;
                continue;
            }
            return false;
        }
        return part == 2 && digits > 0;
    }

    // 打开（或复用）数据库。
    // 首次打开：建目录 → sqlite3_open_v2 → 设置 WAL / busy_timeout → 建工作线程；
    // 已打开：仅把当前窗口加入 owners 并返回同一 dbId。
    // 达到 maxOpenDatabases 上限时返回 SQLITE3_TOO_MANY_OPEN。
    std::shared_future<std::string> Sqlite3Service::open(WindowToken window, const std::string &db_name,
                                                          const std::string &version)
    {
        if (!valid_db_name(db_name) || !valid_version(version))
        {
            return failed<std::string>("SQLITE3_INVALID_ARGUMENT", "invalid dbName or version");
        }

        // Windows / macOS 文件系统大小写不敏感：统一小写，避免 Movie / movie 指向同一文件却建立两个连接。
        const std::string key = to_lower(db_name) + "@" + version;

        // 打开在调用线程内同步完成（桥接线程），用 m_open_mtx 串行化：
        // 幂等语义天然成立，且不需要 “正在打开” 的 future 状态机。
        // 连接全应用共享，owners 记录哪些窗口打开了它。
        std::lock_guard<std::mutex> open_lock(m_open_mtx);
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            const auto it = m_dbs.find(key);
            if (it != m_dbs.end())
            {
                it->second.owners.insert(window); // 其他窗口复用同一连接；同窗口重复 open 幂等
                return ready<std::string>(key);
            }
            if (m_dbs.size() >= m_max_open)
            {
                return failed<std::string>("SQLITE3_TOO_MANY_OPEN",
                                           "too many open databases (max " + std::to_string(m_max_open) + ")");
            }
        }

        std::error_code ec;
        const auto dir = m_data_dir / "sqlite3" / "db";
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            return failed<std::string>("SQLITE3_OPEN_FAILED", "create data directory failed: " + ec.message());
        }

        const auto path = dir / (key + ".db");

        sqlite3 *raw   = nullptr;
        const int rc   = sqlite3_open_v2(utf8_path(path).c_str(), &raw,
                                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK)
        {
            const std::string message = raw != nullptr ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
            if (raw != nullptr)
            {
                sqlite3_close_v2(raw);
            }
            return failed<std::string>("SQLITE3_OPEN_FAILED", message);
        }

        // WAL：读写并发更好；busy_timeout 兜住跨连接瞬时锁竞争。
        sqlite3_exec(raw, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(raw, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);

        auto database = std::make_shared<Sqlite3Database>(raw, utf8_path(path));
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            auto [it, inserted] = m_dbs.emplace(key, Entry{std::move(database), {window}});
            if (!inserted)
            {
                // m_open_mtx 已串行化打开，这里只做防御
                it->second.owners.insert(window);
            }
        }
        return ready<std::string>(key);
    }

    std::shared_ptr<Sqlite3Database> Sqlite3Service::find(WindowToken window, const std::string &db_id) const
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        const auto it = m_dbs.find(db_id);
        if (it == m_dbs.end() || it->second.owners.count(window) == 0)
        {
            throw Sqlite3Error{"SQLITE3_NOT_OPEN", "database not open in this window: " + db_id};
        }
        return it->second.database;
    }

    // 执行单条写/DDL 语句。返回 { changes, lastInsertRowid }；
    // 若 SQL 带 RETURNING，step_all() 会先排空结果集再返回。
    std::shared_future<json> Sqlite3Service::execute(WindowToken window, const std::string &db_id,
                                                     const std::string &sql, const json &params)
    {
        return guard<json>([&] {
            auto database = find(window, db_id); // 提前抛错：库不存在/不属于该窗口时不必进队列

            return database->submit([sql, params](sqlite3 *db) -> json {
                Sqlite3Statement statement(db, sql);
                statement.bind(params);

                if (statement.step_all() != SQLITE_DONE)
                {
                    throw Sqlite3Error{"SQLITE3_EXEC_ERROR", sqlite3_errmsg(db)};
                }

                return json{{"changes", int64_to_json(sqlite3_changes64(db))},
                            {"lastInsertRowid", int64_to_json(sqlite3_last_insert_rowid(db))}};
            });
        });
    }

    // 执行查询：逐行组装对象，列名来自结果集；
    // 同名列表列（如 SELECT a.id, b.id）后者覆盖前者，需要别名区分。
    // 返回 { rows:[{col:value}], columns:[name] }。
    std::shared_future<json> Sqlite3Service::query(WindowToken window, const std::string &db_id,
                                                   const std::string &sql, const json &params)
    {
        return guard<json>([&] {
            auto database = find(window, db_id);

            return database->submit([sql, params](sqlite3 *db) -> json {
                Sqlite3Statement statement(db, sql);
                statement.bind(params);

                const int count = statement.column_count();
                json columns    = json::array();
                for (int i = 0; i < count; ++i)
                {
                    columns.push_back(statement.column_name(i));
                }

                json rows = json::array();
                for (;;)
                {
                    const int rc = sqlite3_step(statement.get());
                    if (rc == SQLITE_DONE)
                    {
                        break;
                    }
                    if (rc != SQLITE_ROW)
                    {
                        throw Sqlite3Error{"SQLITE3_EXEC_ERROR", sqlite3_errmsg(db)};
                    }

                    json row = json::object();
                    for (int i = 0; i < count; ++i)
                    {
                        // 同名列表列（如 SELECT a.id, b.id）后者覆盖前者，需别名区分。
                        row[statement.column_name(i)] = column_value(statement.get(), i);
                    }
                    rows.push_back(std::move(row));
                }

                return json{{"rows", std::move(rows)}, {"columns", std::move(columns)}};
            });
        });
    }

    // 事务：BEGIN IMMEDIATE → 顺序执行 statements → COMMIT；
    // 任一步失败则 ROLLBACK 并重新抛出原始异常。
    // statements 元素为字符串或 { sql, params }。
    std::shared_future<json> Sqlite3Service::transaction(WindowToken window, const std::string &db_id,
                                                         const json &statements)
    {
        if (!statements.is_array() || statements.empty())
        {
            return failed<json>("SQLITE3_INVALID_ARGUMENT", "transaction requires a non-empty array");
        }
        for (const auto &item : statements)
        {
            const bool ok = item.is_string() || (item.is_object() && item.contains("sql") && item["sql"].is_string());
            if (!ok)
            {
                return failed<json>("SQLITE3_INVALID_ARGUMENT",
                                    "transaction item must be a SQL string or { sql: string, params?: object }");
            }
        }

        return guard<json>([&] {
            auto database = find(window, db_id);

            return database->submit([statements](sqlite3 *db) -> json {
                const auto run_one = [db](const std::string &sql, const json &params) {
                    Sqlite3Statement statement(db, sql);
                    statement.bind(params);
                    if (statement.step_all() != SQLITE_DONE)
                    {
                        throw Sqlite3Error{"SQLITE3_EXEC_ERROR", sqlite3_errmsg(db)};
                    }
                };

                exec_raw(db, "BEGIN IMMEDIATE;");
                try
                {
                    for (const auto &item : statements)
                    {
                        if (item.is_string())
                        {
                            run_one(item.get<std::string>(), json::object());
                        }
                        else
                        {
                            run_one(item.at("sql").get<std::string>(), item.value("params", json::object()));
                        }
                    }
                    exec_raw(db, "COMMIT;");
                }
                catch (...)
                {
                    try
                    {
                        exec_raw(db, "ROLLBACK;");
                    }
                    catch (...)
                    {
                        // 回滚失败（例如连接已坏）时保留原始异常
                    }
                    throw;
                }

                return json{{"ok", true}};
            });
        });
    }

    // 关闭当前窗口对 db 的句柄：仅从 owners 移除当前窗口；
    // 若它是最后一个使用者，则摘除路由并在锁外析构连接（可能阻塞）。
    std::shared_future<json> Sqlite3Service::close(WindowToken window, const std::string &db_id)
    {
        std::shared_ptr<Sqlite3Database> closing;
        {
            std::lock_guard<std::mutex> open_lock(m_open_mtx);
            std::lock_guard<std::mutex> lock(m_mtx);
            const auto it = m_dbs.find(db_id);
            if (it == m_dbs.end() || it->second.owners.erase(window) == 0)
            {
                return failed<json>("SQLITE3_NOT_OPEN", "database not open in this window: " + db_id);
            }
            if (it->second.owners.empty())
            {
                // 最后一个使用者：移除路由并稍后真正关闭连接。
                closing = std::move(it->second.database);
                m_dbs.erase(it);
            }
        }

        // 在锁外析构：会排空已入队任务、join 工作线程，再 sqlite3_close_v2；
        // 若仍有其他调用方持有引用，实际关闭发生在最后一个引用释放时。
        closing.reset();
        return ready<json>(json{{"ok", true}});
    }

    // 窗口销毁时注销其全部句柄。与 close 的区别：一次性处理所有 dbId。
    // 最后一个持有者会在本函数内析构连接（可能阻塞），调用方通常放到后台线程。
    void Sqlite3Service::release_all(WindowToken window)
    {
        std::vector<std::shared_ptr<Sqlite3Database>> closing;
        {
            std::lock_guard<std::mutex> open_lock(m_open_mtx);
            std::lock_guard<std::mutex> lock(m_mtx);
            for (auto it = m_dbs.begin(); it != m_dbs.end();)
            {
                it->second.owners.erase(window);
                if (it->second.owners.empty())
                {
                    closing.push_back(std::move(it->second.database));
                    it = m_dbs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // 最后一个持有者：析构时会排空队列并关闭连接（可能阻塞，调用方自行决定是否放到后台线程）。
        closing.clear();
    }
} // namespace waty::detail
