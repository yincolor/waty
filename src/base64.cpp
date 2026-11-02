#include "base64.hpp"

namespace waty::detail
{
    namespace
    {
        const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        int b64_value(char c)
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        }
    } // namespace

    std::string base64_encode(const unsigned char *data, std::size_t size)
    {
        std::string out;
        out.reserve((size + 2) / 3 * 4);
        for (std::size_t i = 0; i < size; i += 3)
        {
            const auto a = data[i];
            const auto b = (i + 1 < size) ? data[i + 1] : 0;
            const auto c = (i + 2 < size) ? data[i + 2] : 0;
            out.push_back(k_b64[a >> 2]);
            out.push_back(k_b64[((a & 0x03) << 4) | (b >> 4)]);
            out.push_back((i + 1 < size) ? k_b64[((b & 0x0f) << 2) | (c >> 6)] : '=');
            out.push_back((i + 2 < size) ? k_b64[c & 0x3f] : '=');
        }
        return out;
    }

    std::string base64_decode(std::string_view input)
    {
        std::string out;
        int buffer = 0;
        int bits   = 0;
        for (const char ch : input)
        {
            if (ch == '=')
            {
                break;
            }
            const int v = b64_value(ch);
            if (v < 0)
            {
                continue;
            }
            buffer = (buffer << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<char>((buffer >> bits) & 0xff));
            }
        }
        return out;
    }
} // namespace waty::detail
