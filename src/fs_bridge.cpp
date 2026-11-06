// 文件系统桥（见 DESIGN.md §8.3）。
//
// 前端 API（需 permissions.fs.read / permissions.fs.write）：
//   waty.fs.readFile(path)            → Uint8Array
//   waty.fs.stat(path)                → { size, mtimeMs, mtimeISO, exists, isFile, isDirectory, type }
//   waty.fs.exists(path)              → boolean
//   waty.fs.createReadStream(path, {highWaterMark}) → ReadableStream<Uint8Array>
//   waty.fs.writeFile(path, data)     → Promise<void>
//   waty.fs.createWriteStream(path)   → WritableStream<Uint8Array>
//
// 协议（通道 "fs"）：
//   请求 { __waty:"fs", id, op, path, ... }；二进制一律 base64。
//   流式读写使用全局 streamId 表，页面必须调用 closeStream（读流 EOF/取消、写流 close/abort）释放。
//
// 错误形状：{ name:"WatyFsError", code, message, details:{ path, syscall } }。
// 线程：所有磁盘 I/O 在临时线程执行，结果 post 回 UI 线程再兑现 Promise。

#include <waty/web_window.hpp>

#include "base64.hpp"
#include "detail.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace waty::detail
{
    namespace
    {
        // -----------------------------------------------------------------
        // 流式读写注册表
        // -----------------------------------------------------------------
        // 所有窗口共享同一张表（key 为全局自增 id）；互斥锁保证多线程访问安全。
        // 读流在 EOF 时自动移除；写流依赖页面 closeStream（析构 ofstream 时落盘）。
        struct StreamEntry
        {
            std::unique_ptr<std::ifstream> in;   // 读流非空
            std::unique_ptr<std::ofstream> out;  // 写流非空
            std::size_t chunk_size = 65536;      // 仅读流使用
        };

        std::mutex g_streams_mutex;
        std::unordered_map<std::uint64_t, std::unique_ptr<StreamEntry>> g_streams;
        std::atomic<std::uint64_t> g_next_stream_id{1};

        // 带稳定错误码与路径的异常，最终序列化为 WatyFsError。
        struct FsError : std::runtime_error
        {
            std::string code;
            std::string path;

            FsError(std::string c, std::string p, std::string m)
                : std::runtime_error(std::move(m)), code(std::move(c)), path(std::move(p))
            {
            }
        };

        nlohmann::json fs_error_json(const FsError &err, std::string_view syscall)
        {
            return nlohmann::json{{"name", "WatyFsError"}, {"code", err.code}, {"message", err.what()},
                                  {"details", {{"path", err.path}, {"syscall", syscall}}}};
        }

        // 组装 stat 结果。文件不存在不抛异常，而是返回 exists=false 的零值对象，
        // 便于前端用同一形状做存在性判断（与 Node.js fs.stat 抛错不同）。
        nlohmann::json stat_json(const std::filesystem::path &path)
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            if (!exists)
            {
                return nlohmann::json{{"size", 0}, {"mtimeMs", 0}, {"mtimeISO", ""}, {"exists", false},
                                      {"isFile", false}, {"isDirectory", false}, {"type", "other"}};
            }

            const bool is_file = std::filesystem::is_regular_file(path, ec);
            const bool is_dir  = std::filesystem::is_directory(path, ec);
            const bool is_link = std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));

            std::uint64_t size = 0;
            if (is_file)
            {
                size = std::filesystem::file_size(path, ec);
            }

            std::uint64_t mtime_ms = 0;
            std::string mtime_iso;
            if (const auto ftime = std::filesystem::last_write_time(path, ec); !ec)
            {
                const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                mtime_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count());

                const std::time_t t = std::chrono::system_clock::to_time_t(sys);
                std::tm tm{};
                gmtime_r(&t, &tm);
                char buf[64];
                if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm) > 0)
                {
                    mtime_iso = buf;
                }
            }

            std::string type = "other";
            if (is_file) type = "file";
            else if (is_dir) type = "directory";
            else if (is_link) type = "symlink";

            return nlohmann::json{{"size", size}, {"mtimeMs", mtime_ms}, {"mtimeISO", mtime_iso}, {"exists", true},
                                  {"isFile", is_file}, {"isDirectory", is_dir}, {"type", type}};
        }

        // 执行一次 fs 操作，返回结果 json；失败抛 FsError。
        // 每个 op 的请求/响应字段见文件头；writeFile 会截断目标文件。
        nlohmann::json do_fs_op(const nlohmann::json &req)
        {
            const std::string op = req.value("op", "");
            const std::string path = req.value("path", "");

            if (op == "readFile")
            {
                std::ifstream file(path, std::ios::binary);
                if (!file)
                {
                    throw FsError("FS_NOT_FOUND", path, "无法打开文件: " + path);
                }
                std::string data((std::istreambuf_iterator<char>(file)), {});
                return nlohmann::json{{"base64", base64_encode(reinterpret_cast<const unsigned char *>(data.data()), data.size())}};
            }

            if (op == "writeFile")
            {
                const std::string data = base64_decode(req.value("base64", ""));
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                if (!file)
                {
                    throw FsError("FS_IO_ERROR", path, "无法写入文件: " + path);
                }
                file.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!file)
                {
                    throw FsError("FS_IO_ERROR", path, "写入文件失败: " + path);
                }
                return nlohmann::json(nullptr);
            }

            if (op == "stat")
            {
                return stat_json(path);
            }

            if (op == "exists")
            {
                std::error_code ec;
                return nlohmann::json(std::filesystem::exists(path, ec));
            }

            if (op == "openReadStream")
            {
                auto entry = std::make_unique<StreamEntry>();
                entry->in = std::make_unique<std::ifstream>(path, std::ios::binary);
                if (!*entry->in)
                {
                    throw FsError("FS_NOT_FOUND", path, "无法打开文件: " + path);
                }
                entry->chunk_size = req.value("highWaterMark", static_cast<std::uint64_t>(65536));
                const auto id = g_next_stream_id.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(g_streams_mutex);
                    g_streams[id] = std::move(entry);
                }
                return nlohmann::json{{"streamId", id}, {"size", req.value("highWaterMark", static_cast<std::uint64_t>(65536))}};
            }

            if (op == "readStreamChunk")
            {
                const auto id = req.value("streamId", static_cast<std::uint64_t>(0));
                std::lock_guard<std::mutex> lock(g_streams_mutex);
                const auto it = g_streams.find(id);
                if (it == g_streams.end() || !it->second->in)
                {
                    return nlohmann::json{{"base64", ""}, {"eof", true}};
                }
                std::string buffer(it->second->chunk_size, '\0');
                it->second->in->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto got = static_cast<std::size_t>(it->second->in->gcount());
                buffer.resize(got);
                const bool eof = got == 0 || it->second->in->eof();
                if (eof)
                {
                    g_streams.erase(it);
                }
                return nlohmann::json{{"base64", base64_encode(reinterpret_cast<const unsigned char *>(buffer.data()), buffer.size())},
                                      {"eof", eof}};
            }

            if (op == "openWriteStream")
            {
                auto entry = std::make_unique<StreamEntry>();
                entry->out = std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc);
                if (!*entry->out)
                {
                    throw FsError("FS_IO_ERROR", path, "无法创建文件: " + path);
                }
                const auto id = g_next_stream_id.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(g_streams_mutex);
                    g_streams[id] = std::move(entry);
                }
                return nlohmann::json{{"streamId", id}};
            }

            if (op == "writeStreamChunk")
            {
                const auto id = req.value("streamId", static_cast<std::uint64_t>(0));
                const std::string data = base64_decode(req.value("base64", ""));
                std::lock_guard<std::mutex> lock(g_streams_mutex);
                const auto it = g_streams.find(id);
                if (it == g_streams.end() || !it->second->out)
                {
                    throw FsError("FS_IO_ERROR", "", "流已关闭");
                }
                it->second->out->write(data.data(), static_cast<std::streamsize>(data.size()));
                return nlohmann::json(nullptr);
            }

            if (op == "closeStream")
            {
                const auto id = req.value("streamId", static_cast<std::uint64_t>(0));
                std::lock_guard<std::mutex> lock(g_streams_mutex);
                g_streams.erase(id);
                return nlohmann::json(nullptr);
            }

            throw FsError("FS_INVALID_ARGUMENT", path, "未知 fs 操作: " + op);
        }
    } // namespace

    // fs 通道入口。捕获 FsError/其它异常后统一 post 回 UI 线程响应；
    // req 按值捕获到线程中，避免调用方释放后悬空。
    void handle_fs_request(WebWindow::Impl &impl, const nlohmann::json &req)
    {
        const std::uint64_t id = req.value("id", static_cast<std::uint64_t>(0));
        WebWindow::Impl *impl_ptr = &impl;
        const auto saucer_app = impl.saucer_app;

        // 文件 I/O 在线程池执行，避免阻塞 UI 线程
        std::thread([impl_ptr, saucer_app, id, req] {
            try
            {
                auto result = do_fs_op(req);
                post_to_ui_thread(saucer_app, [impl_ptr, id, result = std::move(result)]() mutable {
                    respond(*impl_ptr, id, result);
                });
            }
            catch (const FsError &err)
            {
                const auto error = fs_error_json(err, req.value("op", ""));
                post_to_ui_thread(saucer_app, [impl_ptr, id, error] {
                    respond_error(*impl_ptr, id, error);
                });
            }
            catch (const std::exception &err)
            {
                const auto error = nlohmann::json{{"name", "WatyFsError"}, {"code", "FS_IO_ERROR"}, {"message", err.what()},
                                                  {"details", {{"path", req.value("path", "")}, {"syscall", req.value("op", "")}}}};
                post_to_ui_thread(saucer_app, [impl_ptr, id, error] {
                    respond_error(*impl_ptr, id, error);
                });
            }
        }).detach();
    }
} // namespace waty::detail
