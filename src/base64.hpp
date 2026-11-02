#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace waty::detail
{
    /// base64 编码（fs 的二进制传输回退路径，以及 SQLite BLOB 列返回值共用）。
    std::string base64_encode(const unsigned char *data, std::size_t size);

    /// base64 解码（容忍换行/非法字符，遇到 '=' 停止）。
    std::string base64_decode(std::string_view input);
} // namespace waty::detail
