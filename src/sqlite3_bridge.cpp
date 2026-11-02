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

        std::string string_field(const nlohmann::json &req, const char *key)
        {
            if (!req.contains(key) || !req[key].is_string())
            {
                throw Sqlite3Error{"SQLITE3_INVALID_ARGUMENT", std::string(key) + " must be a string"};
            }
            return req[key].get<std::string>();
        }

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
