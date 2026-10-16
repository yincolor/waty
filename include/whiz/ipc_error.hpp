#pragma once

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

        // TODO(doc-gap): 文档提到 details 字段由 IpcError 自定义填充（nlohmann::json），
        // 具体结构在阶段 2 实现 IPC 桥时补全。
        std::optional<std::string> stack;
    };
} // namespace waty
