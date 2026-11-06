// SQLite3 JS 桥（见 DESIGN.md §7.1 / §8.4）。
//
// 前端 API（需 permissions.sqlite3）：
//   waty.sqlite3.openDatabase(dbName, version) → { dbId, execute, query, transaction, close }
//   execute(sql, params)       → { changes, lastInsertRowid }
//   query(sql, params)         → { rows:[...], columns:[...] }
//   transaction(statements)    → { ok:true }；statements 元素为 SQL 字符串
//                                或 { sql, params }，内部 BEGIN IMMEDIATE/COMMIT/ROLLBACK。
//
// 协议（通道 "sqlite3"）：请求 { __waty:"sqlite3", id, op, dbId?, dbName?, version?, sql?, params?, statements? }。
// 错误对象：{ name:"WatySqlite3Error", code: SQLITE3_*, message }。
//
// 线程：请求在临时线程等待 Service 的 shared_future，完成后 post 回 UI 线程响应。
// 数据库连接由 Sqlite3Service 按窗口记账（见 sqlite3_service.hpp）。

#include "detail.hpp"

#include "sqlite3_service.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace waty::detail
{
    namespace
    {
        // 异常 → JS 错误对象。Sqlite3Error 保留自身 code；
        // invalid_argument 映射为 SQLITE3_INVALID_ARGUMENT；其余归为 SQLITE3_INTERNAL。
        nlohmann::json sqlite3_error_from_exception(const std::exception_ptr &ptr)
        {
            try
            {
                std::rethrow_exception(ptr);
            }
            catch (const Sqlite3Error &ex)
            {
                return nlohmann::json{{"name", "WatySqlite3Error"}, {"code", ex.code()}, {"message", ex.what()}};
            }
            catch (const std::invalid_argument &ex)
            {
                return nlohmann::json{{"name", "WatySqlite3Error"},
                                      {"code", "SQLITE3_INVALID_ARGUMENT"},
                                      {"message", ex.what()}};
            }
            catch (const std::exception &ex)
            {
                return nlohmann::json{{"name", "WatySqlite3Error"}, {"code", "SQLITE3_INTERNAL"}, {"message", ex.what()}};
            }
            catch (...)
            {
                return nlohmann::json{{"name", "WatySqlite3Error"},
                                      {"code", "SQLITE3_INTERNAL"},
                                      {"message", "Unknown exception"}};
            }
        }

        // 必需字符串字段：缺失或类型不符抛 Sqlite3Error（最终变成 rejected Promise）。
        std::string string_field(const nlohmann::json &req, const char *key)
        {
            if (!req.contains(key) || !req[key].is_string())
            {
                throw Sqlite3Error{"SQLITE3_INVALID_ARGUMENT", std::string(key) + " must be a string"};
            }
            return req[key].get<std::string>();
        }

        // 可选对象字段：缺失/null 返回空对象，类型不符抛错（params 必须是对象）。
        nlohmann::json object_field(const nlohmann::json &req, const char *key)
        {
            if (!req.contains(key) || req[key].is_null())
            {
                return nlohmann::json::object();
            }
            if (!req[key].is_object())
            {
                throw Sqlite3Error{"SQLITE3_INVALID_ARGUMENT", std::string(key) + " must be an object"};
            }
            return req[key];
        }
    } // namespace

    // sqlite3 通道入口。
    // 所有异步操作都在新线程中等待 Service 的 future，避免阻塞 UI；
    // 但 Service 自身保证同一连接上的 SQL 串行执行。
    void handle_sqlite3_request(WebWindow::Impl &impl, const nlohmann::json &req)
    {
        const auto id = req.value("id", static_cast<std::uint64_t>(0));

        // 防御：权限关闭时即使 JS 被注入也无法执行（正常路径下模块本身不会注入）。
        if (!impl.permissions.sqlite3)
        {
            respond_error(impl, id, nlohmann::json{{"name", "WatySqlite3Error"},
                                                   {"code", "SQLITE3_PERMISSION_DENIED"},
                                                   {"message", "permissions.sqlite3 is disabled"}});
            return;
        }
        if (!impl.sqlite3)
        {
            respond_error(impl, id, nlohmann::json{{"name", "WatySqlite3Error"},
                                                   {"code", "SQLITE3_UNAVAILABLE"},
                                                   {"message", "sqlite3 service is not available"}});
            return;
        }

        // shared_ptr 拷贝进线程：即使窗口/应用先销毁，Sqlite3Service 也存活到请求结束。
        auto service    = impl.sqlite3;
        auto saucer_app = impl.saucer_app;
        auto *impl_ptr  = &impl;
        const auto op   = req.value("op", std::string{});
        const auto window = static_cast<WindowToken>(impl.id);
        auto request    = req;

        std::thread([service, saucer_app, impl_ptr, id, op, window, request] {
            try
            {
                nlohmann::json result;

                if (op == "open")
                {
                    result = service->open(window, string_field(request, "dbName"), string_field(request, "version")).get();
                }
                else if (op == "execute")
                {
                    result = service->execute(window, string_field(request, "dbId"), string_field(request, "sql"),
                                              object_field(request, "params"))
                                 .get();
                }
                else if (op == "query")
                {
                    result = service->query(window, string_field(request, "dbId"), string_field(request, "sql"),
                                            object_field(request, "params"))
                                 .get();
                }
                else if (op == "transaction")
                {
                    if (!request.contains("statements") || !request["statements"].is_array())
                    {
                        throw Sqlite3Error{"SQLITE3_INVALID_ARGUMENT", "statements must be an array"};
                    }
                    result = service->transaction(window, string_field(request, "dbId"), request["statements"]).get();
                }
                else if (op == "close")
                {
                    result = service->close(window, string_field(request, "dbId")).get();
                }
                else
                {
                    throw Sqlite3Error{"SQLITE3_INVALID_ARGUMENT", "unknown op: " + op};
                }

                post_to_ui_thread(saucer_app, [impl_ptr, id, result] { respond(*impl_ptr, id, result); });
            }
            catch (...)
            {
                const auto error = sqlite3_error_from_exception(std::current_exception());
                post_to_ui_thread(saucer_app, [impl_ptr, id, error] { respond_error(*impl_ptr, id, error); });
            }
        }).detach();
    }
} // namespace waty::detail
