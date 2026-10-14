

# whiz 项目维护指南

## 项目概述

whiz 是一个基于 **Saucer + C++23 + CMake** 的轻量级桌面应用框架，目标是提供类似 Electron 的开发体验：用 C++ 组织窗口、WebView 与 IPC，前端构建产物可嵌入二进制实现单文件分发。项目当前版本为 `v3.4.2`，采用 **AGPL-3.0** 许可证。

## 技术栈

| 类别 | 技术 |
|------|------|
| 核心语言 | C++23 |
| 构建系统 | CMake 3.25+ |
| 底层库 | Saucer（跨平台 WebView 封装） |
| 前端嵌入 | `whiz_embed` CMake 函数 |
| 许可证 | GNU Affero General Public License v3.0 |

## 环境要求

### 编译器

- GCC 13+ / Clang 16+ / MSVC 19.35+

### 平台依赖

- **Linux**：`libgtk-4-dev`、`libadwaita-1-dev`、`libwebkitgtk-6.0-dev`、`libjson-glib-dev`
- **Windows**：WebView2 Runtime
- **macOS**：系统自带 Cocoa / WKWebView

### 构建工具

```bash
cmake --version    # 需 3.25+
g++-14 --version   # 或其他支持 C++23 的编译器
```

## 项目结构

```
whiz/
├── cmake/ # CMake 辅助模块
│   └── whiz_embed.cmake
├── web/ # 示例前端页面（参考用）
│   └── index.html
├── DESIGN.md # 完整设计文档
├── CMakeLists.txt # 顶层构建配置
├── LICENSE # AGPL-3.0协议
├── README.md
└── test # 测试代码放在这里，避免污染其他地方;已经有一个whiz-app-template测试项目可以用于测试了，直接改这个即可，把测试样例中连接目标改为编译的新的静态库
```

> 注：`cmake/whiz_embed.cmake` 是提供给使用者的资源嵌入函数，位于 `whiz_SOURCE_DIR/cmake/` 路径下。

## 常用开发命令

### 构建库

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

产物为 `libwhiz.a`。

### Release 构建

```bash
cmake -B build-release -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

### 清理重建

```bash
rm -rf build && cmake -B build -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
```

## 代码更新工作流

### 1. 同步上游

```bash
git remote add upstream https://github.com/yincolor/whiz.git
git fetch upstream
git checkout main
git merge upstream/main
```

### 2. 创建特性分支

```bash
git checkout -b feat/your-feature-name
```

### 3. 修改代码

- **C++ 头文件与源文件**：遵循项目现有的命名和分层习惯。
- **CMake 配置**：若新增源文件，需在对应的 `CMakeLists.txt` 中添加。
- **示例前端**：如需更新 `web/` 目录下的示例页面，保持与嵌入资源函数的兼容性。

### 4. 构建验证

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

确保编译无错误、无新增警告。

### 5. 最小示例验证

构建通过后，使用 README 中的最小示例进行冒烟测试：

```cpp
#include <whiz>

int main() {
    whiz::ApplicationOptions app_opts;
    app_opts.appName = "Whiz Demo";
    whiz::Application app(app_opts);

    whiz::WebWindowOptions win_opts;
    win_opts.title = "Hello whiz";
    win_opts.width = 800;
    win_opts.height = 600;

    auto window = app.createWindow(win_opts);
    window->loadFile("index.html");
    window->show();

    return app.run();
}
```

配套 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_app LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    whiz
    GIT_REPOSITORY https://github.com/yincolor/whiz.git
    GIT_TAG v3.4.2
)
FetchContent_MakeAvailable(whiz)

add_executable(my_app main.cpp)

# whiz_embed 在 FetchContent_MakeAvailable(whiz) 后可直接使用
whiz_embed("${CMAKE_CURRENT_SOURCE_DIR}/web")
target_link_libraries(my_app PRIVATE whiz::whiz whiz::embedded)
```

### 6. 提交前检查

```bash
git diff --stat
git status
```

确认改动范围符合预期，无意外文件变更。

### 7. 提交

建议使用语义化提交信息：

```
feat: 添加 xxx 能力
fix: 修复 xxx 问题
chore: 更新构建配置
docs: 更新 README / DESIGN.md
```

## 代码规范

- **C++ 标准**：严格使用 C++23 特性，不降级到 C++20 或更低。
- **命名风格**：类名用 PascalCase，函数与变量用 camelCase，常量用 UPPER_SNAKE_CASE。
- **头文件保护**：优先使用 `#pragma once`。
- **命名空间**：所有公开 API 位于 `whiz` 命名空间下。
- **CMake**：变量使用 `${VAR}` 形式，目标名使用 `whiz::whiz` 别名。

## 关键 API 参考

### 核心类

| 类名 | 用途 |
|------|------|
| `whiz::Application` | 应用生命周期管理 |
| `whiz::ApplicationOptions` | 应用配置（`appName` 等） |
| `whiz::WebWindow` | 窗口与 WebView 容器 |
| `whiz::WebWindowOptions` | 窗口配置（`title`、`width`、`height`） |

### 关键方法

- `app.createWindow(opts)` — 创建窗口
- `window->loadFile("index.html")` — 从嵌入资源加载页面
- `window->show()` — 显示窗口
- `app.run()` — 启动事件循环

## 权限模型

whiz 采用**显式权限**设计：暴露给 JS 的能力默认关闭，IPC 基础能力默认开启。更新代码时若新增 JS 桥接能力，需在文档中明确标注该能力的权限状态。

## 网络与代理

构建依赖默认从 GitHub 直接下载。若网络受限，需在**本机**配置代理，CMake 脚本不硬编码镜像或离线缓存：

```bash
export https_proxy=http://127.0.0.1:7890
export http_proxy=http://127.0.0.1:7890
git config --global http.proxy http://127.0.0.1:7890
git config --global https.proxy http://127.0.0.1:7890
```

## 文档更新要求

任何影响以下内容的改动，必须同步更新对应文档：

| 变更类型 | 需更新文件 |
|---------|-----------|
| 新增/修改公开 API | `README.md` + `DESIGN.md` |
| 新增平台依赖 | `README.md` 环境要求章节 |
| 版本号变更 | `README.md` 中的 `GIT_TAG` |
| 构建方式变更 | `README.md` 构建库章节 |

## 发布流程（维护者参考）

1. 更新 `README.md` 中的 `GIT_TAG` 为最新版本号。
2. 确认 `CMakeLists.txt` 中项目版本与 tag 一致。
3. 打 tag：`git tag -a vX.Y.Z -m "Release vX.Y.Z"`
4. 推送：`git push origin main --tags`

## 常见问题

**Q：编译报错 `saucer/xxx not found`？**

确认子模块或 FetchContent 已正确拉取。网络受限时配置代理后重试。

**Q：Linux 下缺少 GTK/WebKit 头文件？**

```bash
sudo apt install libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev libjson-glib-dev
```

**Q：想更改嵌入的入口 HTML 文件？**

修改 `whiz_embed("path/to/web")` 中的目录路径，然后重新构建。

