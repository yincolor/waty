#pragma once

#include <optional>
#include <string>
#include <string_view>

// 内部头文件：嵌入资源注册表（供 waty_embed 生成的代码与 src/ 内部使用）。
// 使用者不应直接包含此头文件（属于 waty 内部机制）。
//
// 资源表实现见 src/web_window.cpp：使用 Meyers 单例 + 互斥锁，
// 允许 waty_embed 生成的静态初始化器在 main() 之前安全注册资源。
// 查找 key 为相对嵌入根目录、以 '/' 分隔的路径（如 "assets/app.js"）。
namespace waty::detail
{
    /// 注册一条嵌入式资源（path 为相对嵌入根目录的路径，如 "index.html"）。
    void register_embedded_resource(std::string path, std::string content);

    /// 查找嵌入式资源内容；未找到返回 std::nullopt。
    std::optional<std::string> find_embedded_resource(std::string_view path);
} // namespace waty::detail
