# waty

基于 Saucer + C++23 + CMake 的轻量级桌面应用框架，提供类似 Electron 的开发体验：
用 C++ 组织窗口、WebView 与 IPC，前端构建产物可嵌入二进制实现单文件分发。

## 特性

- 窗口 / WebView / IPC 抽象，贴近 Electron 的开发模型
- 前端资源嵌入（`waty_embed`），入口 HTML 由 C++ 侧决定
- 常用桌面能力：文件系统、系统信息、日志、崩溃捕获、原生对话框
- 权限显式化：暴露给 JS 的能力默认关闭（IPC 基础能力默认开启）
- 作为 CMake 库，可通过 `FetchContent` 集成

## 环境要求

- C++23 编译器（GCC 13+ / Clang 16+ / MSVC 19.35+）
- CMake 3.25+
- Linux 开发包：`libgtk-4-dev`、`libadwaita-1-dev`、`libwebkitgtk-6.0-dev`、`libjson-glib-dev`
- Windows：WebView2 Runtime
- macOS：系统自带 Cocoa / WKWebView

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
    GIT_TAG v3.4.2)
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

## 网络说明

依赖默认从 GitHub 直接下载。网络受限时请由使用者在本机配置代理，CMake 脚本不硬编码镜像或离线缓存：

```bash
export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890
git config --global http.proxy http://127.0.0.1:7890
git config --global https.proxy http://127.0.0.1:7890
```

## 文档

完整设计文档见 [DESIGN.md](DESIGN.md)。

## 许可证

本项目采用 [GNU Affero General Public License v3.0 (AGPL-3.0)](https://www.gnu.org/licenses/agpl-3.0.html)。
