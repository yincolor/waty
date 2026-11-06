#pragma once

// SQLite 语句封装与 JSON 值转换（仅 src/ 内部使用）。
//
// 类型映射：
//   JSON null    ↔ SQL NULL
//   bool         ↔ INTEGER 0/1
//   integer      ↔ INTEGER（须在 int64 范围内）
//   float        ↔ REAL
//   string       ↔ TEXT
//   object/array → TEXT（JSON 文本，不递归展开）
//   BLOB         → base64 字符串（读取时）
//
// 整数精度：超过 JS Number 安全整数范围（2^53-1）的 INTEGER 以字符串返回。
// 语句约束：execute/query 只接受“恰好一条语句”，多条请用 transaction。

#include "base64.hpp"
#include "sqlite3_database.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace waty::detail
{
    /// JS Number 能精确表示的最大整数；超出时以字符串返回，避免精度丢失。
    inline constexpr std::int64_t k_max_safe_integer = 9'007'199'254'740'991LL;

    // 把单个 JSON 值绑定到指定参数位置（1 起始）。
    // 文本统一用 SQLITE_TRANSIENT：SQLite 会拷贝一份，调用方无需保证生命周期。
    inline void bind_value(sqlite3_stmt *stmt, int index, const nlohmann::json &value)
    {
        if (value.is_null())
        {
            sqlite3_bind_null(stmt, index);
            return;
        }
        if (value.is_boolean())
        {
            sqlite3_bind_int(stmt, index, value.get<bool>() ? 1 : 0);
            return;
        }
        // 先判断无符号：nlohmann 的 is_number_integer() 同时覆盖有符号/无符号，
        // 若先按 int64 取值，超过 INT64_MAX 的无符号数会越界。
        if (value.is_number_unsigned())
        {
            const auto u = value.get<std::uint64_t>();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                throw Sqlite3Error{"SQLITE3_INVALID_ARGUMENT", "integer out of int64 range"};
            }
            sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(u));
            return;
        }
        if (value.is_number_integer())
        {
            sqlite3_bind_int64(stmt, index, value.get<std::int64_t>());
            return;
        }
        if (value.is_number_float())
        {
            sqlite3_bind_double(stmt, index, value.get<double>());
            return;
        }
        if (value.is_string())
        {
            const auto &s = value.get_ref<const std::string &>();
            sqlite3_bind_text(stmt, index, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
            return;
        }

        // 对象 / 数组：以 JSON 文本写入 TEXT 列（不递归展开）。
        const std::string text = value.dump();
        sqlite3_bind_text(stmt, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
    }

    /// 命名参数绑定：键名可用 `:name` / `@name` / `$name` 任一写法（逐个尝试）。
    /// SQL 中不存在的键被忽略；SQL 中存在但未提供的命名参数保持 NULL。
    inline void bind_params(sqlite3_stmt *stmt, const nlohmann::json &params)
    {
        if (params.is_null())
        {
            return;
        }
        if (!params.is_object())
        {
            throw Sqlite3Error{"SQLITE3_INVALID_ARGUMENT", "params must be an object"};
        }
        for (auto it = params.begin(); it != params.end(); ++it)
        {
            // 同一个参数名在 SQL 里可能写作 :x / @x / $x，必须逐个前缀查索引。
            const int index = [&] {
                for (const char prefix : {':', '@', '$'})
                {
                    const std::string placeholder = std::string(1, prefix) + it.key();
                    const int found               = sqlite3_bind_parameter_index(stmt, placeholder.c_str());
                    if (found != 0)
                    {
                        return found;
                    }
                }
                return 0;
            }();
            if (index == 0)
            {
                continue; // SQL 里没有这个命名参数 → 忽略（避免拼写错误直接报错打断调用）
            }
            bind_value(stmt, index, it.value());
        }
    }

    // 把结果集某列转换为 JSON。BLOB 使用 base64（前端需自行解码）。
    // 列类型由 sqlite3_column_type 动态决定（SQLite 是动态类型）。
    inline nlohmann::json column_value(sqlite3_stmt *stmt, int index)
    {
        switch (sqlite3_column_type(stmt, index))
        {
            case SQLITE_INTEGER: {
                const auto value = sqlite3_column_int64(stmt, index);
                if (value > k_max_safe_integer || value < -k_max_safe_integer)
                {
                    return std::to_string(value);
                }
                return value;
            }
            case SQLITE_FLOAT:
                return sqlite3_column_double(stmt, index);
            case SQLITE_TEXT: {
                const auto *text  = sqlite3_column_text(stmt, index);
                const int   bytes = sqlite3_column_bytes(stmt, index);
                if (text == nullptr)
                {
                    return std::string{};
                }
                return std::string(reinterpret_cast<const char *>(text), static_cast<std::size_t>(bytes));
            }
            case SQLITE_BLOB: {
                const auto *data  = static_cast<const unsigned char *>(sqlite3_column_blob(stmt, index));
                const int   bytes = sqlite3_column_bytes(stmt, index);
                return base64_encode(data, static_cast<std::size_t>(bytes));
            }
            default:
                return nullptr;
        }
    }

    /// 单条语句 RAII 封装；构造时即校验“恰好一条语句”。
    /// 析构自动 sqlite3_finalize；禁止拷贝，避免重复 finalize。
    class Sqlite3Statement
    {
      public:
        Sqlite3Statement(sqlite3 *db, std::string_view sql)
        {
            const std::string text{sql};
            const char *tail = nullptr;

            sqlite3_stmt *stmt = nullptr;
            if (sqlite3_prepare_v2(db, text.c_str(), static_cast<int>(text.size()), &stmt, &tail) != SQLITE_OK)
            {
                const std::string message = sqlite3_errmsg(db);
                if (stmt != nullptr)
                {
                    sqlite3_finalize(stmt);
                }
                throw Sqlite3Error{"SQLITE3_EXEC_ERROR", message};
            }
            if (stmt == nullptr)
            {
                throw Sqlite3Error{"SQLITE3_EMPTY_SQL", "SQL is empty (or contains only comments)"};
            }

            const auto only_whitespace = [](const char *p) {
                for (; p != nullptr && *p != '\0'; ++p)
                {
                    if (!std::isspace(static_cast<unsigned char>(*p)))
                    {
                        return false;
                    }
                }
                return true;
            };
            if (!only_whitespace(tail))
            {
                sqlite3_finalize(stmt);
                throw Sqlite3Error{"SQLITE3_MULTI_STATEMENT",
                                   "execute/query accept exactly one statement; use transaction for multiple"};
            }

            m_stmt = stmt;
        }

        ~Sqlite3Statement()
        {
            if (m_stmt != nullptr)
            {
                sqlite3_finalize(m_stmt);
            }
        }

        Sqlite3Statement(const Sqlite3Statement &)            = delete;
        Sqlite3Statement &operator=(const Sqlite3Statement &) = delete;

        void bind(const nlohmann::json &params) { bind_params(m_stmt, params); }

        /// 执行到 SQLITE_DONE；对返回行的语句（如带 RETURNING）会先排空结果集。
        int step_all()
        {
            int rc = sqlite3_step(m_stmt);
            while (rc == SQLITE_ROW)
            {
                rc = sqlite3_step(m_stmt);
            }
            return rc;
        }

        sqlite3_stmt *get() const noexcept { return m_stmt; }
        int column_count() const noexcept { return sqlite3_column_count(m_stmt); }
        const char *column_name(int index) const noexcept { return sqlite3_column_name(m_stmt, index); }

      private:
        sqlite3_stmt *m_stmt = nullptr;
    };
} // namespace waty::detail
