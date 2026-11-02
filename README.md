# waty

基于 Saucer + C++23 + CMake 的轻量级桌面应用框架，提供类似 Electron 的开发体验：
用 C++ 组织窗口、WebView 与 IPC，前端构建产物可嵌入二进制实现单文件分发。

## 特性

- 窗口 / WebView / IPC 抽象，贴近 Electron 的开发模型
- 前端资源嵌入（`waty_embed`），入口 HTML 由 C++ 侧决定
- 常用桌面能力：文件系统、系统信息、日志、崩溃捕获、原生对话框、多数据库 SQLite
- 权限显式化：暴露给 JS 的能力默认关闭（IPC 基础能力默认开启）
- 作为 CMake 库，可通过 `FetchContent` 集成

## 环境要求

- C++23 编译器（GCC 13+ / Clang 16+ / MSVC 19.35+）
- CMake 3.25+
- Linux 开发包：`libgtk-4-dev`、`libadwaita-1-dev`、`libwebkitgtk-6.0-dev`、`libjson-glib-dev`
- Windows：WebView2 Runtime
- macOS：系统自带 Cocoa / WKWebView
- SQLite 3.49.1（通过 FetchContent 自动获取，CI 需可访问 `github.com` 与 `sqlite.org`）

## 权限一览

JS 侧能力默认关闭（IPC 除外），需要在 `ApplicationOptions::permissions` 中显式开启：

| 权限 | 默认 | 开启后可用 |
|---|---|---|
| `permissions.ipc` | `true` | `window.waty.ipc` |
| `permissions.os` | `false` | `window.waty.os`（`platform` / `getPath`） |
| `permissions.window` | `false` | `window.waty.window`（窗口控制、DevTools） |
| `permissions.devtools` | `false` | `window.waty.window.openDevTools()` |
| `permissions.log` | `false` | JS `console.*` 转发到 C++ 日志 |
| `permissions.dialog` | `false` | `window.waty.dialog` |
| `permissions.fs.read` / `.write` | `false` | `window.waty.fs` |
| `permissions.sqlite3` | `false` | `window.waty.sqlite3` |

## 构建库

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

产物为 `libwaty.a`。

## 最小示例

`main.cpp`：

```cpp
#include <waty/application.hpp>

int main()
{
    waty::ApplicationOptions app_opts;
    app_opts.appName = "Waty_Demo";

    waty::Application app(app_opts);

    waty::WebWindowOptions win_opts;
    win_opts.title  = "Hello waty";
    win_opts.width  = 800;
    win_opts.height = 600;

    auto window = app.createWindow(win_opts);
    window->loadFile("index.html");   // 从嵌入资源加载
    window->show();

    return app.run();
}
```

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_app LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(waty
    GIT_REPOSITORY https://github.com/<you>/waty.git
    GIT_TAG v3.5.0)
FetchContent_MakeAvailable(waty)

add_executable(my_app main.cpp)

# waty_embed 在 FetchContent_MakeAvailable(waty) 后可直接使用
waty_embed("${CMAKE_CURRENT_SOURCE_DIR}/web")

target_link_libraries(my_app PRIVATE waty::waty waty::embedded)
```

在项目里准备 `web/index.html`（本仓库 `web/` 目录提供了一个可参考的示例页面），然后：

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
./build/my_app
```

## 读取嵌入资源

C++ 侧可用 `Application::readEmbeddedFile` 读取嵌入资源（含图片、字体等二进制文件）的原始字节：

```cpp
if (auto bytes = app.readEmbeddedFile("assets/icon.png"))
{
    // *bytes 为 std::vector<std::uint8_t>，是原始字节的副本
}
else
{
    // 未找到对应嵌入资源
}
```

`path` 为相对嵌入根目录的路径（与 `loadFile` 的 `entry` 同一命名空间）；未找到时返回 `std::nullopt`。

## IPC

C++ 侧通过 `WebWindow::ipcHandle` 响应前端的 `window.waty.ipc.invoke(event, ...)`：

- **每个事件名只能注册一个 handler。**
- **同一事件名重复注册会抛出 `std::logic_error`**（异常信息包含事件名）。
- **通过返回的 `Subscription::unsubscribe()` 取消注册**；取消后该事件名可再次注册。
- 回调在线程池执行，返回 `std::future<nlohmann::json>` 以异步响应。

## Sqlite3

`window.waty.sqlite3` 提供多数据库 SQLite 访问。默认关闭，需显式开启权限：

```cpp
app_opts.permissions.sqlite3 = true;  // 默认 false

// 可选：调整同时打开的数据库上限（1..1024，默认 64）
app_opts.sqlite3.maxOpenDatabases = 64;
```

`openDatabase(dbName, version)` 的规则：

- `dbName` 匹配 `^[A-Za-z0-9_]{1,64}$`，大小写不敏感（内部统一小写）。
- `version` 匹配 `^\d{1,3}\.\d{1,3}\.\d{1,3}$`（如 `0.0.1`）。不同 `version` 是**独立数据库**，Waty 不做 schema 迁移。
- 同一 `(dbName, version)` 全应用只打开一条连接；文件位于 `<AppData>/sqlite3/db/<dbName>@<version>.db`。
- **连接全应用共享，句柄按窗口记账**：多个窗口打开同一库时共用同一条连接；某窗口 `close()` 只注销它自己的句柄（该窗口后续操作报 `SQLITE3_NOT_OPEN`），其他窗口继续正常使用。最后一个窗口 `close` 或窗口关闭/销毁时才真正关闭连接。
- 未打开过该库的窗口即使拿到 `dbId` 也不能直接操作（`SQLITE3_NOT_OPEN`）。

```js
const db = await window.waty.sqlite3.openDatabase("movie", "0.0.1"); // <AppData>/sqlite3/db/movie@0.0.1.db
await db.execute("CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, name TEXT)");
await db.execute("INSERT INTO t(name) VALUES (:name)", { name: "hello" });
const { rows } = await db.query("SELECT id, name FROM t");
await db.transaction([
    { sql: "INSERT INTO t(name) VALUES (:name)", params: { name: "a" } }
]);
await db.close();
```

- 每个数据库使用独立连接与工作线程，库与库之间互不阻塞；同一数据库上的操作严格串行（跨窗口也一样）。
- 同一窗口重复 `openDatabase` 幂等（仍是一个句柄），一次 `close()` 即注销该窗口的持有。
- `execute` / `query` 仅接受单条语句（多语句 → `SQLITE3_MULTI_STATEMENT`，请改用 `transaction`）；`execute` 返回 `{ changes, lastInsertRowid }`。
- `params` 只支持命名参数对象（`:name` / `@name` / `$name`）；数组/对象参数会以 JSON 文本写入，BLOB 查询结果以 base64 返回。
- 错误对象为 `{ name: "WatySqlite3Error", code, message }`，错误码包括 `SQLITE3_INVALID_ARGUMENT`、`SQLITE3_PERMISSION_DENIED`、`SQLITE3_TOO_MANY_OPEN`、`SQLITE3_OPEN_FAILED`、`SQLITE3_NOT_OPEN`、`SQLITE3_EMPTY_SQL`、`SQLITE3_MULTI_STATEMENT`、`SQLITE3_EXEC_ERROR` 等，完整列表见 `DESIGN.md` 8.6.4。
- 默认 `journal_mode=WAL`、`busy_timeout=5000`；`foreign_keys` 保持 SQLite 默认关闭，需要时请执行 `PRAGMA foreign_keys=ON`。

## 网络说明

依赖默认从 GitHub 直接下载。网络受限时请由使用者在本机配置代理，CMake 脚本不硬编码镜像或离线缓存：

```bash
export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890
git config --global http.proxy http://127.0.0.1:7890
git config --global https.proxy http://127.0.0.1:7890
```

## 测试

SQLite 单测默认不构建，开启后通过 CTest 运行（不依赖 WebView / GTK）：

```bash
cmake -B build -DWATY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

测试覆盖参数校验、打开幂等/并发、打开上限、窗口级 close 与释放、WAL、事务回滚、BLOB/base64、大整数、多库隔离、并发写以及 `SQLITE_THREADSAFE=1` 断言。

## 文档

完整设计文档见 [DESIGN.md](DESIGN.md)（Sqlite3 详见 8.6，配置见 6.2 / 6.2.1，线程模型见 7.1）。

## 许可证

本项目采用 [GNU Affero General Public License v3.0 (AGPL-3.0)](https://www.gnu.org/licenses/agpl-3.0.html)。
