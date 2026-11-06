#pragma once

// IPC 错误结构（见 DESIGN.md §8.1.2）。
//
// 仅作为文档/兼容类型使用：当前 C++ 桥在捕获异常后直接构造 JSON 返回给前端，
// 形状为 { name, code, message }，前端可统一用 err.code 分支处理。

#include <optional>
#include <string>

namespace waty
{
    /// IPC 错误（见文档 8.1.2）。
    /// 当 C++ 侧 ipcHandle 回调抛出异常时，JS 侧 Promise 会以该结构对应的错误信息 reject。
    struct IpcError
    {
        std::string code;     // 推荐错误码，如 IPC_HANDLER_NOT_FOUND / IPC_INVALID_ARGUMENT
        std::string message;  // 人类可读错误信息

        // 错误堆栈（平台/运行时相关），前端可用于调试；未提供时为 nullopt。
        std::optional<std::string> stack;
    };
} // namespace waty
