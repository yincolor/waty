// waty base64 单测（不依赖 WebView / Saucer）。
//
// base64 是 fs 桥与 SQLite BLOB 的二进制传输格式，这里覆盖：
//   * RFC 4648 标准向量；
//   * 空输入、1/2/3 字节边界的 '=' 填充；
//   * 解码时对换行/空格/非法字符的容忍；
//   * 全字节范围（0x00..0xFF，含 NUL）的往返一致性。
//
// 构建：cmake -B build -DWATY_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build

#include "base64.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    int g_failures = 0;

    void expect(bool condition, const std::string &what)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
    }

    void expect_eq(std::string_view actual, std::string_view expected, const std::string &what)
    {
        if (actual != expected)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s (actual='%s', expected='%s')\n", what.c_str(),
                         std::string(actual).c_str(), std::string(expected).c_str());
        }
    }

    std::string encode(std::string_view input)
    {
        return waty::detail::base64_encode(
            reinterpret_cast<const unsigned char *>(input.data()), input.size());
    }

    void expect_decode(std::string_view input, std::string_view expected, const std::string &what)
    {
        expect_eq(waty::detail::base64_decode(input), expected, what);
    }
} // namespace

int main()
{
    // 1. RFC 4648 标准向量
    expect_eq(encode(""), "", "encode empty");
    expect_eq(encode("f"), "Zg==", "encode 1 byte");
    expect_eq(encode("fo"), "Zm8=", "encode 2 bytes");
    expect_eq(encode("foo"), "Zm9v", "encode 3 bytes");
    expect_eq(encode("foob"), "Zm9vYg==", "encode 4 bytes");
    expect_eq(encode("fooba"), "Zm9vYmE=", "encode 5 bytes");
    expect_eq(encode("foobar"), "Zm9vYmFy", "encode 6 bytes");

    expect_decode("", "", "decode empty");
    expect_decode("Zg==", "f", "decode 1 byte");
    expect_decode("Zm8=", "fo", "decode 2 bytes");
    expect_decode("Zm9v", "foo", "decode 3 bytes");
    expect_decode("Zm9vYg==", "foob", "decode 4 bytes");
    expect_decode("Zm9vYmE=", "fooba", "decode 5 bytes");
    expect_decode("Zm9vYmFy", "foobar", "decode 6 bytes");

    // 2. 解码容忍换行/空格等非字母表字符，遇到 '=' 立即停止
    expect_decode("Zm9v\nYmFy", "foobar", "decode ignores newlines");
    expect_decode("Zm9v YmFy", "foobar", "decode ignores spaces");
    expect_decode("!!Zm9v??", "foo", "decode skips invalid characters");
    expect_decode("!!!!", "", "decode all-invalid input");
    expect_decode("Zg==", "f", "decode stops at padding");
    expect_decode("Zg==extra", "f", "decode ignores bytes after padding");
    expect_decode("Zm9v====", "foo", "decode tolerates extra padding");
    expect_decode("=", "", "decode single padding");
    expect_decode("====", "", "decode only padding");

    // 3. 全字节范围往返（含 0x00，验证按长度而非 C 字符串处理）
    std::string all;
    all.resize(256);
    for (int i = 0; i < 256; ++i)
    {
        all[static_cast<std::size_t>(i)] = static_cast<char>(i);
    }
    const auto encoded = encode(all);
    expect(encoded.size() == 344, "encode 256 bytes produces 344 chars");
    expect_eq(waty::detail::base64_decode(encoded), all, "decode(encode(0x00..0xFF)) round-trips");

    // 4. 二进制数据（含内嵌 NUL）单独再测一次
    const std::string with_nul("a\0b\0c", 5);
    expect_eq(waty::detail::base64_decode(encode(with_nul)), with_nul, "embedded NUL round-trips");

    if (g_failures == 0)
    {
        std::printf("base64 tests passed\n");
    }
    else
    {
        std::fprintf(stderr, "%d base64 test(s) failed\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
