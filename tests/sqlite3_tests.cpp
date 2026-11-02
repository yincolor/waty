// waty Sqlite3 单测（不依赖 WebView / Saucer）。
// 构建：cmake -B build -DWATY_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
//
// 窗口用整数 token 模拟（真实实现传 WebWindow::id）：
// 连接全应用共享，但 open/close 按窗口记账。

#include "sqlite3_service.hpp"
#include "sqlite3_statement.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using nlohmann::json;
    using waty::detail::Sqlite3Error;
    using waty::detail::WindowToken;

    constexpr WindowToken k_w1 = 1;
    constexpr WindowToken k_w2 = 2;
    constexpr WindowToken k_w3 = 103; // 刻意避开并发 open 使用的 1..8
    constexpr WindowToken k_w4 = 104;

    int g_failures = 0;

    void expect(bool condition, const std::string &what)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
    }

    template <typename F>
    void expect_error(F &&call, const std::string &code, const std::string &what)
    {
        try
        {
            call();
        }
        catch (const Sqlite3Error &ex)
        {
            expect(ex.code() == code, what + " (code=" + ex.code() + ", expected " + code + ", message=" + ex.what() + ")");
            return;
        }
        catch (const std::exception &ex)
        {
            expect(false, what + " (unexpected exception: " + ex.what() + ")");
            return;
        }
        expect(false, what + " (no exception)");
    }

    void expect_rejected(std::shared_future<std::string> future, const std::string &code, const std::string &what)
    {
        expect_error([&] { future.get(); }, code, what);
    }

    std::filesystem::path make_temp_dir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto dir = std::filesystem::temp_directory_path() /
                   ("waty_sqlite3_tests_" + std::to_string(stamp));
        std::filesystem::create_directories(dir);
        return dir;
    }
} // namespace

int main()
{
    const auto root = make_temp_dir();
    auto service    = std::make_shared<waty::detail::Sqlite3Service>(root, 64);

    // 1. dbName 校验
    expect(!waty::detail::Sqlite3Service::valid_db_name(""), "empty dbName rejected");
    expect(!waty::detail::Sqlite3Service::valid_db_name("../evil"), "path traversal dbName rejected");
    expect(!waty::detail::Sqlite3Service::valid_db_name("a-b"), "dash dbName rejected");
    expect(!waty::detail::Sqlite3Service::valid_db_name(std::string(65, 'a')), "65-char dbName rejected");
    expect(waty::detail::Sqlite3Service::valid_db_name("movie"), "movie dbName accepted");
    expect(waty::detail::Sqlite3Service::valid_db_name("Movie_2"), "Movie_2 dbName accepted");

    // 2. version 校验
    expect(!waty::detail::Sqlite3Service::valid_version("1.0"), "1.0 version rejected");
    expect(!waty::detail::Sqlite3Service::valid_version("1.0.0.0"), "1.0.0.0 version rejected");
    expect(!waty::detail::Sqlite3Service::valid_version("1..0"), "1..0 version rejected");
    expect(!waty::detail::Sqlite3Service::valid_version("1000.0.0"), "1000.0.0 version rejected");
    expect(waty::detail::Sqlite3Service::valid_version("0.0.1"), "0.0.1 version accepted");
    expect(waty::detail::Sqlite3Service::valid_version("999.999.999"), "999.999.999 version accepted");

    // 非法参数不产生文件
    expect_rejected(service->open(k_w1, "../evil", "0.0.1"), "SQLITE3_INVALID_ARGUMENT", "open rejects traversal name");

    // 3. open 幂等（8 个窗口并发打开同一个库 → 同一 dbId、同一条连接）
    std::vector<std::future<std::string>> opens;
    for (int i = 0; i < 8; ++i)
    {
        opens.push_back(std::async(std::launch::async, [&, i] { return service->open(static_cast<WindowToken>(i + 1), "movie", "0.0.1").get(); }));
    }
    std::string movie_id;
    for (auto &f : opens)
    {
        const auto id = f.get();
        if (movie_id.empty())
        {
            movie_id = id;
        }
        expect(id == movie_id, "concurrent open returns the same dbId");
    }
    expect(movie_id == "movie@0.0.1", "dbId format <name>@<version>");

    // 4. 大小写归一
    expect(service->open(k_w2, "Movie", "0.0.1").get() == movie_id, "dbName is case-insensitive");

    // 5. 未打开该库的窗口不能直接使用 dbId
    expect_error([&] { service->execute(k_w3, movie_id, "SELECT 1", json::object()).get(); },
                 "SQLITE3_NOT_OPEN", "window that never opened the db is rejected");

    // 6. 文件落点 + WAL
    const auto db_file = root / "sqlite3" / "db" / "movie@0.0.1.db";
    expect(std::filesystem::exists(db_file), "db file created under <AppData>/sqlite3/db");
    expect(service->query(k_w1, movie_id, "PRAGMA journal_mode", json::object()).get()["rows"][0]["journal_mode"] == "wal",
           "journal_mode is WAL");

    // 7. execute 语义
    const auto create = service->execute(k_w1, movie_id, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, data BLOB)", json::object()).get();
    expect(create["changes"] == 0, "CREATE reports 0 changes");

    const auto insert = service->execute(k_w1, movie_id, "INSERT INTO t(name) VALUES (:name)", {{"name", "hello"}}).get();
    expect(insert["changes"] == 1, "INSERT reports 1 change");
    expect(insert["lastInsertRowid"] == 1, "lastInsertRowid is 1");
    expect(std::filesystem::exists(db_file.string() + "-wal"), "WAL file exists after first write");

    expect_error([&] { service->execute(k_w1, movie_id, "CREATE TABLE x(a); INSERT INTO x VALUES(1)", json::object()).get(); },
                 "SQLITE3_MULTI_STATEMENT", "multi-statement rejected");
    expect_error([&] { service->execute(k_w1, movie_id, "   -- only comment", json::object()).get(); },
                 "SQLITE3_EMPTY_SQL", "comment-only SQL rejected");
    expect_error([&] { service->execute(k_w1, movie_id, "", json::object()).get(); }, "SQLITE3_EMPTY_SQL", "empty SQL rejected");
    expect_error([&] { service->execute(k_w1, movie_id, "INSERT INTO missing VALUES(1)", json::object()).get(); },
                 "SQLITE3_EXEC_ERROR", "syntax/schema error reported");

    // 8. query：columns 顺序、大整数、BLOB、NULL
    service->execute(k_w1, movie_id, "INSERT INTO t(name, data) VALUES ('blob', x'000102')", json::object()).get();
    service->execute(k_w1, movie_id, "INSERT INTO t(name) VALUES (NULL)", json::object()).get();
    service->execute(k_w1, movie_id, "INSERT INTO t(id, name) VALUES (9007199254740993, 'big')", json::object()).get();

    const auto result = service->query(k_w1, movie_id, "SELECT id, name, data FROM t ORDER BY id", json::object()).get();
    expect(result["columns"] == json::array({"id", "name", "data"}), "query returns columns in order");
    const auto &rows = result["rows"];
    expect(rows.size() == 4, "query returns all rows");
    expect(rows[0]["name"] == "hello", "row 0 name bound by named parameter");
    expect(rows[1]["data"] == "AAEC", "BLOB column returned as base64");
    expect(rows[2]["name"].is_null(), "NULL column returned as null");
    expect(rows[3]["id"] == "9007199254740993", "INTEGER beyond 2^53 returned as string");

    // 9. params 类型映射
    const auto param_echo = service->query(
        k_w1, movie_id,
        "SELECT :b AS b, :s AS s, :o AS o, :a AS a, :n AS n",
        {{"b", true}, {"s", "text"}, {"o", json{{"k", 1}}}, {"a", json::array({1, 2})}, {"n", nullptr}}).get();
    const auto &echo = param_echo["rows"][0];
    expect(echo["b"] == 1, "bool bound as 1");
    expect(echo["s"] == "text", "string bound as text");
    expect(echo["o"] == "{\"k\":1}", "object bound as JSON text");
    expect(echo["a"] == "[1,2]", "array bound as JSON text");
    expect(echo["n"].is_null(), "null bound as NULL");
    expect_error([&] { service->execute(k_w1, movie_id, "SELECT :x", json::array({1})).get(); },
                 "SQLITE3_INVALID_ARGUMENT", "params array rejected");

    // 10. transaction
    service->execute(k_w1, movie_id, "CREATE TABLE tx(id INTEGER PRIMARY KEY, v TEXT UNIQUE)", json::object()).get();
    service->transaction(k_w1, movie_id, json::array({
        "INSERT INTO tx(v) VALUES ('a')",
        json{{"sql", "INSERT INTO tx(v) VALUES (:v)"}, {"params", json{{"v", "b"}}}}
    })).get();
    expect(service->query(k_w1, movie_id, "SELECT COUNT(*) AS c FROM tx", json::object()).get()["rows"][0]["c"] == 2,
           "transaction commits all statements");
    expect_error([&] { service->transaction(k_w1, movie_id, json::array({
        "INSERT INTO tx(v) VALUES ('c')",
        "INSERT INTO tx(v) VALUES ('a')"
    })).get(); }, "SQLITE3_EXEC_ERROR", "transaction failure surfaces error");
    expect(service->query(k_w1, movie_id, "SELECT COUNT(*) AS c FROM tx", json::object()).get()["rows"][0]["c"] == 2,
           "transaction rolls back on failure");
    expect_error([&] { service->transaction(k_w1, movie_id, json::array()).get(); },
                 "SQLITE3_INVALID_ARGUMENT", "empty transaction rejected");

    // 11. 上限按“不同库”计数，窗口级引用不额外占额度
    auto small = std::make_shared<waty::detail::Sqlite3Service>(root / "small", 1);
    small->open(k_w1, "one", "0.0.1").get();
    expect(small->open(k_w2, "one", "0.0.1").get() == "one@0.0.1", "second window shares the same single connection");
    expect_rejected(small->open(k_w1, "two", "0.0.1"), "SQLITE3_TOO_MANY_OPEN", "maxOpenDatabases counts distinct dbs");
    small->close(k_w1, "one@0.0.1").get();
    expect_rejected(small->open(k_w1, "two", "0.0.1"), "SQLITE3_TOO_MANY_OPEN",
                    "single close does not free the slot while another window holds it");
    small->close(k_w2, "one@0.0.1").get();
    expect(small->open(k_w1, "two", "0.0.1").get() == "two@0.0.1", "last close frees the slot");

    // 12. 多库隔离：movie 忙碌时 anime 不受影响
    const auto anime_id = service->open(k_w1, "anime", "0.0.1").get();
    service->execute(k_w1, anime_id, "CREATE TABLE a(id INTEGER PRIMARY KEY)", json::object()).get();

    auto slow = service->query(k_w1, movie_id,
                               "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<3000000) "
                               "SELECT count(*) AS c FROM c",
                               json::object());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = std::chrono::steady_clock::now();
    service->execute(k_w1, anime_id, "INSERT INTO a DEFAULT VALUES", json::object()).get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    slow.get();
    expect(elapsed.count() < 500, "other database stays responsive while one is busy (" +
                                      std::to_string(elapsed.count()) + "ms)");

    // 13. 窗口级 close：只影响调用窗口，其他窗口继续可用
    expect(service->close(k_w2, movie_id).get()["ok"] == true, "window 2 close returns ok");
    expect_error([&] { service->execute(k_w2, movie_id, "SELECT 1", json::object()).get(); },
                 "SQLITE3_NOT_OPEN", "closed window can no longer use the db");
    expect(service->query(k_w1, movie_id, "SELECT COUNT(*) AS c FROM t", json::object()).get()["rows"][0]["c"] == 4,
           "other window keeps working after window 2 closes");
    expect_error([&] { service->close(k_w3, movie_id).get(); }, "SQLITE3_NOT_OPEN", "close by non-owner rejected");

    // 14. 最后一个使用者关闭后才真正关闭连接；之后可重新打开并读到原数据
    expect(service->close(k_w1, anime_id).get()["ok"] == true, "window 1 closes anime");
    expect_error([&] { service->execute(k_w1, anime_id, "SELECT 1", json::object()).get(); },
                 "SQLITE3_NOT_OPEN", "operation after close rejected");
    expect_error([&] { service->close(k_w1, anime_id).get(); }, "SQLITE3_NOT_OPEN", "double close rejected");
    expect(service->open(k_w1, "anime", "0.0.1").get() == anime_id, "reopen returns the same dbId");
    expect(service->query(k_w1, anime_id, "SELECT COUNT(*) AS c FROM a", json::object()).get()["rows"][0]["c"] == 1,
           "data persists after reopen");

    // 15. 同一窗口重复 open 幂等；一次 close 即注销该窗口（窗口级布尔持有）
    service->open(k_w3, "shared", "0.0.1").get();
    service->open(k_w3, "shared", "0.0.1").get();
    service->open(k_w4, "shared", "0.0.1").get();
    service->execute(k_w3, "shared@0.0.1", "CREATE TABLE s(v TEXT)", json::object()).get();
    service->execute(k_w4, "shared@0.0.1", "INSERT INTO s VALUES ('x')", json::object()).get();
    service->close(k_w3, "shared@0.0.1").get();
    expect_error([&] { service->execute(k_w3, "shared@0.0.1", "SELECT 1", json::object()).get(); },
                 "SQLITE3_NOT_OPEN", "single close releases this window's handle");
    expect(service->query(k_w4, "shared@0.0.1", "SELECT COUNT(*) AS c FROM s", json::object()).get()["rows"][0]["c"] == 1,
           "window 4 still holds the shared connection");

    // 16. 窗口销毁（release_all）：注销该窗口全部句柄，最后持有者才真正关闭
    service->open(k_w2, "shared", "0.0.1").get();
    service->release_all(k_w2); // 模拟窗口销毁
    expect_error([&] { service->execute(k_w2, "shared@0.0.1", "SELECT 1", json::object()).get(); },
                 "SQLITE3_NOT_OPEN", "release_all removes this window's handles");
    service->release_all(k_w4);
    expect_error([&] { service->execute(k_w4, "shared@0.0.1", "SELECT 1", json::object()).get(); },
                 "SQLITE3_NOT_OPEN", "last owner released → connection closed");

    // 17. 线程安全宏
    expect(sqlite3_threadsafe() == 1, "SQLITE_THREADSAFE=1 (CMake must set sqlite3_ENABLE_THREADSAFE=ON)");

    // 18. 并发写同一库
    const auto counter_id = service->open(k_w1, "counter", "0.0.1").get();
    service->execute(k_w1, counter_id, "CREATE TABLE c(id INTEGER PRIMARY KEY)", json::object()).get();
    {
        std::vector<std::thread> threads;
        std::atomic<int> errors{0};
        for (int i = 0; i < 8; ++i)
        {
            threads.emplace_back([&] {
                for (int j = 0; j < 200; ++j)
                {
                    try
                    {
                        service->execute(k_w1, counter_id, "INSERT INTO c DEFAULT VALUES", json::object()).get();
                    }
                    catch (...)
                    {
                        ++errors;
                    }
                }
            });
        }
        for (auto &t : threads)
        {
            t.join();
        }
        expect(errors == 0, "concurrent execute does not error");
    }
    expect(service->query(k_w1, counter_id, "SELECT COUNT(*) AS n FROM c", json::object()).get()["rows"][0]["n"] == 1600,
           "concurrent execute writes 1600 rows");

    if (g_failures == 0)
    {
        std::printf("sqlite3 tests passed\n");
    }
    else
    {
        std::fprintf(stderr, "%d sqlite3 test(s) failed\n", g_failures);
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return g_failures == 0 ? 0 : 1;
}
