#pragma once

// SQLite3 服务层（仅 src/ 内部使用，经 sqlite3_bridge.cpp 暴露给 JS）。
//
// 职责：
//   * 校验 dbName / version，并把 "<lower(dbName)>@<version>" 作为全局 dbId；
//   * 管理 dbId → Sqlite3Database 的路由与窗口所有权（owners）；
//   * 把所有操作包装为 std::shared_future，保证“绝不同步抛”契约。
//
// 数据库文件位置：<AppData>/sqlite3/db/<dbId>.db。
// 同一 dbId 全应用只打开一次；不同窗口各自持有一个逻辑句柄，
// 最后一个持有者 close/release 时才真正关闭连接。
//
// 锁顺序：m_open_mtx → m_mtx（打开/关闭时），其它路径只拿 m_mtx。

#include "sqlite3_database.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace waty::detail
{
    /// 窗口句柄标识（使用 WebWindow::id；窗口关闭/销毁时调用 release_all 注销）。
    using WindowToken = std::uint64_t;

    /// 库路由：dbId（"<db_name>@<version>"）→ 共享连接 + 持有该连接的窗口集合。
    ///
    /// 连接是全应用共享的：同一 dbId 只会打开一次。但“打开/关闭”按窗口记账：
    /// 每个窗口各自持有一个逻辑句柄；某个窗口 close 只影响它自己，
    /// 最后一个窗口 close 或窗口销毁时才真正关闭 sqlite3 连接。
    ///
    /// 所有方法返回 std::shared_future 且从不同步抛异常（失败写入 future）。
    class Sqlite3Service
    {
      public:
        Sqlite3Service(std::filesystem::path data_dir, std::size_t max_open);

        Sqlite3Service(const Sqlite3Service &)            = delete;
        Sqlite3Service &operator=(const Sqlite3Service &) = delete;

        std::shared_future<std::string> open(WindowToken window, const std::string &db_name,
                                             const std::string &version);
        std::shared_future<nlohmann::json> execute(WindowToken window, const std::string &db_id,
                                                   const std::string &sql, const nlohmann::json &params);
        std::shared_future<nlohmann::json> query(WindowToken window, const std::string &db_id,
                                                 const std::string &sql, const nlohmann::json &params);
        std::shared_future<nlohmann::json> transaction(WindowToken window, const std::string &db_id,
                                                       const nlohmann::json &statements);
        std::shared_future<nlohmann::json> close(WindowToken window, const std::string &db_id);

        /// 注销某窗口持有的全部数据库句柄（窗口关闭/销毁时调用）。
        /// 某个库在注销后没有其他窗口持有，则真正关闭连接（排空队列 + join + sqlite3_close_v2）。
        void release_all(WindowToken window);

        // 校验函数（暴露给单测）
        static bool valid_db_name(std::string_view name);
        static bool valid_version(std::string_view version);

      private:
        struct Entry
        {
            std::shared_ptr<Sqlite3Database> database;
            std::unordered_set<WindowToken> owners;
        };

        // 查找并校验所有权；库不存在或不属于该窗口时抛 SQLITE3_NOT_OPEN。
        std::shared_ptr<Sqlite3Database> find(WindowToken window, const std::string &db_id) const;

        std::filesystem::path m_data_dir;
        std::size_t m_max_open;

        mutable std::mutex m_mtx; // 保护 m_dbs
        std::unordered_map<std::string, Entry> m_dbs;
        std::mutex m_open_mtx; // 串行化打开/关闭（磁盘操作）
    };
} // namespace waty::detail
