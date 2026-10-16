#pragma once

#include <optional>
#include <string>
#include <string_view>

// 内部头文件：供 waty_embed 生成的代码调用，向嵌入式资源注册表注入内容。
// 使用者不应直接包含此头文件（属于 waty 内部机制）。
namespace waty::detail
{
    /// 注册一条嵌入式资源（path 为相对嵌入根目录的路径，如 "index.html"）。
    void register_embedded_resource(std::string path, std::string content);

    /// 查找嵌入式资源内容；未找到返回 std::nullopt。
    std::optional<std::string> find_embedded_resource(std::string_view path);
} // namespace waty::detail
