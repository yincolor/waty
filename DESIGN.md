# Whiz 项目文档 v3.4.2

> 基于 Saucer + C++23 + CMake 的轻量级桌面应用框架，提供类似 Electron 的开发体验。

---

## 目录

1. 项目简介
2. 设计理念
3. 环境要求
4. 快速开始
5. 资源嵌入机制
6. 配置选项
7. C++ API
8. JavaScript API
9. IPC 异步处理与数据转换
10. TypeScript 全局声明
11. 调试与日志
12. 与 Electron 对照
13. 目录结构
14. 设计边界
15. 安全说明

---

## 1. 项目简介

**Whiz** 是一个构建在 [Saucer](https://github.com/saucer/saucer) 之上的 C++ 桌面应用框架。它将 Saucer 的底层能力（窗口、WebView、C++/JS 互操作）包装成贴近应用开发直觉的 API，让 C++ 开发者能用熟悉的语法快速构建 Web 技术驱动的桌面应用，同时保留 C++ 的性能与体积优势。

**Whiz 做什么：**

- 提供类似 Electron 的窗口、WebView、IPC 抽象
- 把前端构建产物嵌入二进制，实现单文件分发
- 提供常用的桌面能力：文件系统、系统信息、日志、崩溃捕获、原生对话框
- 作为 CMake 库，通过 `FetchContent` 即可被其他项目集成

---

## 2. 设计理念

| 原则 | 说明 |
|---|---|
| **封装 + 常用桌面能力** | 核心仍是 API 映射和资源嵌入，同时提供必要的桌面能力，但不重新发明轮子 |
| **职责分离** | CMake 管嵌入，C++ 管加载，前端构建交给使用者 |
| **单文件分发** | 前端资源编译进二进制，运行时零外部依赖 |
| **权限显式化** | 所有暴露给 JS 的能力都需要在 C++ 侧显式配置权限，默认关闭。**例外：IPC 是基础能力，默认开启。** |
| **原生优先** | 对话框等系统级交互直接调用平台原生 API，保证外观与行为与操作系统一致 |

---

## 3. 环境要求

- C++23 编译器（GCC 13+ / Clang 16+ / MSVC 19.35+）
- CMake 3.20+（Saucer v8.x 实际最低要求）
- Saucer v8.x（通过 FetchContent 自动获取）
- nlohmann/json（通过 FetchContent 自动获取）

### 3.1 平台依赖

**Windows**：需 WebView2 Runtime（Windows 11 预装，Windows 10 通常已随 Edge 更新安装）。

**macOS**：使用系统自带的 Cocoa 和 WKWebView，无需额外安装。

**Linux**：
- 后端为 **GTK4 + WebKitGTK 6.0**
- **推荐 GTK 4.10+**：以获得 `GtkAlertDialog` 支持，`showMessageBox` 将使用原生现代 API
- **GTK 4.10 以下**：`showMessageBox` 回退到 `gtk_message_dialog_new`，功能可用，外观可能略有差异
- 需安装：`gtk4`、`webkitgtk-6.0`

> **关于 saucer/desktop**：Saucer 的官方模块（包括 `desktop`、`pdf`、`loop`）随主库一起发布，版本与 Saucer 主库保持一致。使用 Saucer v8.x 时，`saucer/desktop` 自动为对应版本，无需单独指定版本号。自 Saucer v5.0.0 起，桌面相关功能正式移入 `saucer/desktop` 模块。

---

## 4. 快速开始

### 4.1 集成方式：使用者的 CMakeLists.txt

```cmake
include(FetchContent)

FetchContent_Declare(whiz
    GIT_REPOSITORY https://github.com/yourname/whiz.git
    GIT_TAG v3.4.2)          # 固定版本 tag，避免 main 分支不稳定
FetchContent_MakeAvailable(whiz)

add_executable(my_app main.cpp)

# 将前端构建产物生成独立资源目标（默认 whiz::embedded）
whiz_embed("${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist")

target_link_libraries(my_app PRIVATE whiz::whiz whiz::embedded)
```

`whiz_embed` 是 Whiz 提供的 CMake 函数，用于把指定目录整个嵌入到独立资源目标中。入口 HTML 由 C++ 侧在运行时决定。

### 4.2 最小 main.cpp

```cpp
#include <whiz/application.hpp>

int main() {
    whiz::ApplicationOptions appOpts;
    appOpts.appName = "My App";

    whiz::Application app(appOpts);

    whiz::WebWindowOptions winOpts;
    winOpts.title = "Main";
    winOpts.width = 800;
    winOpts.height = 600;

    auto ww = app.createWindow(winOpts);
    ww->loadFile("index.html");   // 入口由 C++ 决定
    ww->show();

    return app.run();
}
```

### 4.3 完整示例：含权限与窗口选项

```cpp
#include <whiz/application.hpp>

int main() {
    whiz::ApplicationOptions appOpts;
    appOpts.appName = "My App";
    appOpts.icon = "assets/icon.png";          // 可指向嵌入资源中的相对路径，或相对于可执行文件目录的文件系统路径
    appOpts.permissions.os = true;             // 启用 JS 系统信息 + getPath
    appOpts.permissions.dialog = true;         // 启用 JS 原生对话框
    appOpts.permissions.fs.read = true;        // 启用 JS 文件读取
    appOpts.permissions.fs.write = false;      // 默认关闭写
    appOpts.logFile = "/home/user/data/app.log"; // 建议绝对路径，可用 getPath 拼接

    whiz::Application app(appOpts);

    whiz::WebWindowOptions winOpts;
    winOpts.title = "Main";
    winOpts.width = 1024;
    winOpts.height = 768;
    winOpts.minWidth = 640;
    winOpts.minHeight = 480;
    winOpts.resizable = true;

    auto ww = app.createWindow(winOpts);
    ww->loadFile("page/index.html");           // 加载嵌入资源
    ww->show();

    return app.run();
}
```

#### 4.3.1 对话框调用示例

> 以下示例假定已 `#include <whiz/dialog.hpp>`（或 `<whiz/application.hpp>` 已包含该头文件；具体以实际头文件依赖为准）。

```cpp
// 打开文件选择对话框（必须在 UI 线程调用）
whiz::OpenDialogOptions openOpts;
openOpts.title = "选择图片";
openOpts.defaultPath = "/home/user/pictures";
openOpts.filters = {{"Images", {"png", "jpg", "jpeg"}}};

ww->showOpenDialog(openOpts, [](std::optional<std::vector<std::string>> paths) {
    if (paths) {
        // 用户选择了文件，paths 为路径列表
    } else {
        // 用户取消
    }
});

// 显示消息框（必须在 UI 线程调用）
whiz::MessageBoxOptions msgOpts;
msgOpts.type = whiz::MessageBoxType::Question;
msgOpts.title = "确认";
msgOpts.message = "确定要删除吗？";
msgOpts.buttons = {"取消", "删除"};
msgOpts.defaultButtonIndex = 1;
msgOpts.cancelButtonIndex = 0;

ww->showMessageBox(msgOpts, [](whiz::MessageBoxResult result) {
    if (result.response == 1) {
        // 用户点击了“删除”
    } else if (result.response == 0) {
        // 用户点击了“取消”，或按了 Esc 键
    } else {
        // 用户通过系统关闭方式（如标题栏关闭按钮）关闭了对话框
    }
});
```

### 4.4 单实例锁标准示例

```cpp
int main() {
    whiz::ApplicationOptions appOpts;
    appOpts.appName = "My App";
    whiz::Application app(appOpts);

    if (!app.requestSingleInstanceLock()) {
        app.quit();          // 未获得锁，立即退出
        return 0;
    }

    // ... 创建窗口等

    return app.run();
}
```

### 4.5 使用者侧三步

1. 构建前端，产物落到 `frontend/dist/`
2. CMake 通过 `FetchContent` 引入 Whiz，并调用 `whiz_embed` 嵌入资源
3. C++ 里配置 `ApplicationOptions`、创建 `WebWindowOptions`，调用：
   `app.createWindow(...) → loadFile("...") → show() → app.run()`

入口 HTML 的位置、多页面结构、资源路径，全部由使用者的 C++ 代码和前端工程决定。Whiz 只负责把它们嵌进去并跑起来。

### 4.6 核心对象

**`Application`**：管理生命周期、窗口集合、全局事件、权限配置、事件循环任务。

**`WebWindow`**：封装窗口 + WebView，提供 `loadFile` / `loadURL` / `setFullscreen` / `setWindowFrameState` / `setSize` / `setMinSize` / `setResizable` / `id` / `windowEventHandle` / `ipcHandle` / `emit` / `show` / `openDevTools` / `showOpenDialog` / `showSaveDialog` / `showMessageBox` 以及完整的窗口控制方法。

---

## 5. 资源嵌入机制

Whiz 提供 CMake 函数 `whiz_embed`：

```cmake
whiz_embed(<directory> [NAME <name>])
```

1. 构建时扫描 `<directory>` 下的所有文件
2. 生成 C++ 源文件，并创建一个独立资源目标（默认 `whiz::embedded`，可用 `NAME` 自定义，如 `whiz::ui`）
3. 使用者通过 `target_link_libraries(my_app PRIVATE whiz::embedded)` 链接
4. 注册 `embedded://root/` scheme 供 WebView 访问

一个资源目标可被多个可执行目标链接，资源只编译一次。

**关键特性：**

- 整个根目录被嵌入，相对路径（如：`../css/theme.css`、`js/page_home.js`）自动解析
- 入口 HTML 可以是任意相对路径：`index.html`、`page/index.html`、`app/main.html`
- 支持多入口，运行时按需切换
- 禁止通过 `../` 跳出嵌入根目录；`loadFile` 会先规范化路径，再判断是否仍在嵌入根目录内。`../` 在根目录内是允许的（如 `page/../index.html`），规范化后跳出根的路径会被拒绝。

**图标路径解析优先级：**

当设置 `ApplicationOptions.icon` 或 `WebWindowOptions.icon` 时，Whiz 按以下顺序查找：

1. 嵌入资源中的相对路径
2. 相对于可执行文件目录的文件系统路径

如果两者都找不到，将使用系统默认应用图标，并记录日志，不会导致程序退出。

---

## 6. 配置选项

### 6.1 FsPermissionOptions

```cpp
struct FsPermissionOptions {
    bool read = false;
    bool write = false;
};
```

> Whiz 不对文件路径做白名单限制。路径访问安全由操作系统或容器工具负责。启用前请评估安全风险。

### 6.2 PermissionOptions

```cpp
struct PermissionOptions {
    bool ipc = true;           // IPC 是基础能力，默认开启的例外
    bool os = false;           // 是否启用 os 模块（含 getPath）
    bool window = false;       // 是否启用 JS 窗口控制
    bool devtools = false;     // 是否允许打开 DevTools
    bool log = false;          // 是否转发 JS console 到 C++ 日志
    bool dialog = false;       // 是否启用对话框模块（open / save / message）
    FsPermissionOptions fs;    // 文件系统权限
};
```

> `dialog` 权限默认关闭，与 `os`、`window` 的权限模型保持一致。

### 6.3 ApplicationOptions

```cpp
struct ApplicationOptions {
    std::string_view appName;                 // 应用名称 或 项目名称
    std::string_view icon;                    // 图标路径，可指向嵌入资源中的相对路径，或相对于可执行文件目录的文件系统路径；留空则应用无自定义图标，使用系统默认图标。
    std::optional<std::string_view> logFile;  // 日志文件路径。相对路径相对于可执行文件目录；规范上建议传绝对路径，可用 getPath 拼接
    PermissionOptions permissions;            // JS 权限配置
};
```

> **字符串生命周期**：所有 `std::string_view` 字段在内部会立即拷贝为 `std::string`，调用者无需担心生命周期问题。

### 6.4 WebWindowOptions

```cpp
struct WebWindowOptions {
    std::string_view title;                  // 窗口标题
    int width = 800;                         // 窗口宽度，默认 800
    int height = 600;                        // 窗口高度，默认 600
    int minWidth = 0;                        // 窗口最小宽度，默认 0
    int minHeight = 0;                       // 窗口最小高度，默认 0
    bool resizable = true;                   // 是否可调整尺寸
    bool fullscreen = false;                 // 是否全屏，默认 false
    bool frame = true;                       // 是否存在窗口边框，默认 true
    std::weak_ptr<WebWindow> parentWebWindow; // 父窗口，使用 weak_ptr 避免生命周期问题
    std::string_view icon;                   // 图标路径，可指向嵌入资源中的相对路径，或相对于可执行文件目录的文件系统路径；留空则继承 ApplicationOptions.icon；若 ApplicationOptions.icon 也为空，则使用系统默认图标。
};
```

> `frame = false` 时窗口无标题栏/边框，但保留阴影与调整大小能力（对应 Saucer 的 `decoration::partial`）。

**尺寸校验规则：**

- 宽度、高度、最小宽度、最小高度为负值 → 抛出 `std::invalid_argument`
- 宽度、高度超过屏幕分辨率 → 抛出 `std::out_of_range`
- 最小尺寸超过屏幕分辨率 → 抛出 `std::out_of_range`

### 6.5 对话框选项

```cpp
struct FileFilter {
    std::string name;                    // 显示名称，如 "Images"
    std::vector<std::string> extensions; // 扩展名，如 {"png", "jpg", "jpeg"}
};

struct OpenDialogOptions {
    std::string_view title;              // 对话框标题
    std::string_view defaultPath;        // 默认路径，仅支持文件系统路径，不支持嵌入资源相对路径
    bool multiSelect = false;            // 是否允许多选
    bool showHidden = false;             // 是否显示隐藏文件
    bool directory = false;              // 是否选择目录而非文件
    std::vector<FileFilter> filters;     // 文件类型过滤器
};

struct SaveDialogOptions {
    std::string_view title;
    std::string_view defaultPath;        // 默认路径，仅支持文件系统路径，不支持嵌入资源相对路径
    std::string_view defaultName;        // 默认文件名
    std::vector<FileFilter> filters;
};

enum class MessageBoxType {
    None,
    Info,
    Warning,
    Error,
    Question
};

struct MessageBoxOptions {
    std::string_view title;
    std::string_view message;
    std::string_view detail;                 // 可选，详细信息
    MessageBoxType type = MessageBoxType::None;
    std::vector<std::string> buttons;        // 按钮文本列表，至少一个
    int defaultButtonIndex = 0;              // 默认按钮索引（回车触发）
    int cancelButtonIndex = -1;              // 取消按钮索引（Esc 触发），-1 表示无取消按钮
    std::string_view checkboxLabel;          // 可选，复选框文本；为空表示不显示复选框，此时 checkboxChecked 被忽略
    bool checkboxChecked = false;            // 可选，复选框初始状态
    bool modal = true;                       // 是否模态
};

struct MessageBoxResult {
    int response;            // 用户点击的按钮索引（0 起始）；
                             // 通过标题栏关闭对话框时为 -1；
                             // 按 Esc 且未设置 cancelButtonIndex 时同样为 -1；
                             // 按 Esc 且设置了 cancelButtonIndex 时返回该索引；
                             // 点击 cancelButtonIndex 对应按钮时返回该索引
    bool checkboxChecked;    // 复选框最终状态；未启用复选框时为 false
};
```

> **校验规则补充**：`MessageBoxOptions.buttons` 为空时抛出 `std::invalid_argument`。`defaultButtonIndex` 越界时抛出 `std::out_of_range`；`cancelButtonIndex` 不为 -1 且越界时抛出 `std::out_of_range`。

> **`checkboxChecked` 与 `checkboxLabel` 的关系**：`checkboxLabel` 为空或未提供时，`checkboxChecked` 被忽略，返回值中的 `checkboxChecked` 恒为 `false`。

> **`cancelButtonIndex` 与 `defaultButtonIndex` 的关系**：二者指向同一按钮时，行为由平台原生对话框决定；建议避免这种配置。

> **模态语义**：`modal = true` 时，对话框会阻止与创建它的 `WebWindow` 的交互；`modal = false` 时，对话框不阻止任何窗口交互，但仍归属于创建它的窗口，窗口关闭时对话框一并关闭；对话框不会独立于窗口存在。

---

## 7. C++ API

### 7.1 线程模型

Whiz 的线程模型与 Saucer 一致，核心规则只有三条：

- **写操作在 UI 线程**：创建窗口、`loadFile` / `loadURL`、`setSize` / `show` / `hide` / `close` 等所有写方法，必须在 UI 线程调用。
- **耗时操作在线程池**：`ipcHandle` 回调在线程池执行，可以执行阻塞操作，不会卡住 UI。
- **跨线程通信走 `emit`**：任意线程都可以调用 `emit`，内部会投递到目标窗口的 UI 线程执行。

使用者无需自己创建 UI 线程，也无需自己管理事件循环；只要遵循这三条即可。

#### 7.1.1 什么是 UI 线程

**UI 线程 = 创建 `Application` 对象的线程 = 调用 `app.run()` 的线程**，通常是 `main()` 所在的主线程。

`app.run()` 内部进入 Saucer 的事件循环：

| 平台 | 事件循环 |
|---|---|
| Windows | Win32 消息循环（`GetMessage` / `DispatchMessage`） |
| macOS | `NSApplication` run loop |
| Linux | GTK 主循环 |

所有原生窗口操作（创建、显示、调整大小、事件分发）都必须在 UI 线程执行，这是操作系统的硬性要求。Whiz 不做线程切换，谁调用 `app.run()`，谁就是 UI 线程。

#### 7.1.2 线程视图

```
┌─────────────────────────────────────────────────────┐
│  UI 线程（通常是 main 线程）                          │
│  ─────────────────────────────────                   │
│  main() → Application → createWindow → loadFile     │
│         → show → app.run()  ← 进入事件循环           │
│                                                     │
│  事件循环中处理：                                     │
│  · 窗口事件 → windowEventHandle 回调                 │
│  · 应用事件 → app.on 回调                            │
│  · 事件循环任务 → onEventLoopTick 回调               │
│  · emit 投递的消息 → 推送给 JS                       │
│  · IPC future 完成 → resolve/reject JS Promise       │
│  · 对话框回调 → showOpenDialog / showMessageBox 等   │
│  · 写方法调用                                        │
└─────────────────────────────────────────────────────┘
           │                        ▲
           │ emit                   │ future 完成
           │ (线程安全)              │ (回到 UI 线程)
           ▼                        │
┌──────────────────────┐  ┌──────────────────────────┐
│  任意线程             │  │  线程池                   │
│  ─────────           │  │  ────────                 │
│  · emit              │  │  · ipcHandle 回调执行      │
│  · unsubscribe       │  │  · std::async / 异步任务   │
│  · 窗口只读方法       │  │  · 不能操作 UI/Saucer      │
└──────────────────────┘  └──────────────────────────┘
```

#### 7.1.3 各操作所在线程

| 操作 | 所在线程 | 说明 |
|---|---|---|
| `Application` 构造 | UI 线程 | 必须在调用 `app.run()` 的线程 |
| `app.createWindow` | UI 线程 | 创建原生窗口 |
| `app.quit` / `app.run` | UI 线程 | 事件循环相关 |
| `app.on` 回调 | UI 线程 | 应用事件 |
| `app.onEventLoopTick` 回调 | UI 线程 | 每轮事件循环执行一次 |
| `app.getPath` | 任意线程 | 建议实现为线程安全 |
| `app.getWindow / app.windows` |任意线程| 建议加锁返回快照 |
| `app.setIcon` | UI 线程 | 操作原生窗口 |
| `ww->loadFile` / `loadURL` | UI 线程 | 操作 WebView |
| `ww->show` / `hide` / `close` | UI 线程 | 操作原生窗口 |
| `ww->setSize` / `setPosition` 等写方法 | UI 线程 | 操作原生窗口 |
| `ww->getSize` / `title` 等只读方法 | 任意线程 | 读取内部缓存 |
| `ww->windowEventHandle` 回调 | UI 线程 | 窗口事件 |
| `ww->ipcHandle` 回调 | 线程池 | 允许耗时操作 |
| `ww->emit` | 任意线程 | 内部投递到 UI 线程 |
| `ww->showOpenDialog` / `showSaveDialog` / `showMessageBox` | UI 线程 | 操作原生对话框 |
| `Subscription::unsubscribe` | 任意线程 | 加锁，安全 |

#### 7.1.4 详细规则

- **IPC 请求回调（`ipcHandle`）默认在线程池执行**。你可以在回调中执行耗时操作，不会阻塞 UI。
- IPC 回调**不能直接操作 UI / Saucer 对象**。如需更新窗口，请使用 `emit` 或通过 UI 线程调度。
- 若 IPC 回调返回 `std::future<nlohmann::json>`，Whiz 会在 future 完成后，**回到 UI 线程** resolve / reject 对应的 JS Promise。
- **JS 侧超时或取消（`createInvoker`）后，Promise 已 reject；C++ future 仍会执行到完成，但结果会被丢弃，不再尝试 resolve/reject。** Whiz 当前不提供 C++ 侧的取消令牌；如需真正取消 C++ 任务，请由使用者自行实现。
- **`emit` 是线程安全的**。任意线程可调用，内部会投递到目标窗口的 UI 线程执行。当目标窗口已关闭时，`emit` 静默丢弃消息并记录警告，不抛异常。
- **`emit` 仅推送给调用它的 `WebWindow` 实例对应的 JS 环境，不会广播到其他窗口。** 如需广播，需遍历 `app.windows()` 逐个调用 `emit`。
- **窗口事件监听回调（`windowEventHandle`）在 UI 线程执行**。
- **应用事件监听回调在 UI 线程执行**。
- **事件循环任务（`onEventLoopTick`）在 UI 线程执行，每轮事件循环触发一次。** 回调中应避免耗时操作，否则会阻塞 UI 与 WebView。需要执行耗时逻辑时，应投递到线程池（如 `std::async`），并通过 `emit` 回到 UI 线程更新界面。任务回调中**禁止**注册或取消 `onEventLoopTick` 任务，违反会被检测并触发断言失败，程序退出（详见 7.3.3）。
- **对话框方法（`showOpenDialog` / `showSaveDialog` / `showMessageBox`）必须在 UI 线程调用**，回调也在 UI 线程执行。原生对话框 API 依赖 UI 线程的消息循环，跨线程调用会被拒绝。
- **窗口只读方法**（`getSize` / `getPosition` / `title` / `isMaximized` / `isMinimized` / `isVisible` / `id`）内部缓存窗口状态并加锁，任意线程可安全读取。缓存在窗口事件（`resize` / `move` / `maximize` / `minimize` / `show` / `hide` 等）触发时更新，因此刚变更后立即读取可能存在极小延迟。写方法（`setSize` / `setPosition` / `maximize` / `minimize` / `close` / `show` / `hide` 等）必须在 UI 线程调用。
- **`Subscription::unsubscribe` 线程安全**。若回调正在执行，`unsubscribe` 不会中断当前回调；当前回调执行完成后不再触发后续回调。

#### 7.1.5 典型时序

```
main()                                 [UI 线程]
 ├─ Application app(opts)              [UI 线程] 构造
 ├─ app.createWindow(winOpts)          [UI 线程] 创建原生窗口
 ├─ ww->loadFile("index.html")         [UI 线程] 加载嵌入资源
 ├─ ww->show()                         [UI 线程] 显示窗口
 └─ app.run()                          [UI 线程] 进入事件循环
      │
      ├─ JS 触发 invoke("get-user") ──→ [线程池]
      │                                    ipcHandle 回调执行
      │                                    返回 std::future<json>
      │   ←── future 完成后回到 UI 线程 ────┘
      │       resolve JS Promise
      │
      ├─ 用户调整窗口大小
      │   └─ windowEventHandle("resize")  [UI 线程]
      │
      ├─ JS 调用 dialog.showOpenDialog
      │   └─ 内部调度到 UI 线程 → 弹出原生对话框 [UI 线程]
      │       └─ 用户选择后 → 回调 → resolve JS Promise
      │
      ├─ 另一个线程调用 ww->emit(...)      [任意线程]
      │   └─ 内部投递到 UI 线程 → 推送给 JS
      │
      ├─ onEventLoopTick 回调              [UI 线程] 每轮执行
      │
      └─ app.quit() → 事件循环退出         [UI 线程]
```

#### 7.1.6 使用建议

1. **`main()` 里创建 `Application` 和窗口，直接 `app.run()`，不要自己另起线程跑事件循环。**
2. **IPC 回调里不要直接操作 `WebWindow`。** 若需要更新窗口，用 `emit` 让 JS 处理，或通过 UI 线程调度。
3. **只读方法可以放心在任意线程调用。** 写方法要回到 UI 线程。
4. **`emit` 是最常用的跨线程通信方式**，任意线程调用，内部投递到 UI 线程。
5. **`Subscription` 可以在任意线程 `unsubscribe`**，但正在执行的回调不会被中断，只是之后不再触发。
6. **对话框方法必须在 UI 线程调用**。由于原生对话框阻塞 UI 线程消息循环，建议从 JS 侧异步触发（`await window.whiz.dialog.showOpenDialog(...)`），不要在 C++ 侧同步等待。
7. **`onEventLoopTick` 回调中不要做耗时操作**。它是通用扩展点，用于将外部事件源接入 Whiz 的事件循环；如果外部事件源本身能以 fd 形式接入平台主循环（例如 Linux 上的 D-Bus fd、Windows 上的隐藏窗口消息、macOS 上的 `NSStatusItem`），应优先走平台事件源机制，而不是周期性 tick。

### 7.2 通用类型

#### 7.2.1 Subscription

```cpp
class Subscription {
public:
    Subscription() = default;
    Subscription(Subscription&&) noexcept;
    Subscription& operator=(Subscription&&) noexcept;
    ~Subscription();

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    // 取消订阅。取消后回调不再触发。可跨线程调用。
    // 若回调正在执行，unsubscribe 不会中断当前回调；当前回调执行完成后不再触发后续回调。
    void unsubscribe();

    // 是否仍然有效
    bool valid() const;
};
```

`Subscription` 不可拷贝，可移动。**只有显式调用 `unsubscribe()` 才会取消订阅；析构不会自动取消。** 若需要取消，请持有该句柄并在合适时机调用 `unsubscribe()`。

若回调正在执行，`unsubscribe()` 不会中断当前回调，仅阻止后续触发。

#### 7.2.2 Event

```cpp
class Event {
public:
    // 阻止默认行为（如应用退出、关闭窗口、window-all-closed 的默认退出）
    void preventDefault();

    // 是否已阻止默认行为
    bool defaultPrevented() const;
};
```

### 7.3 Application

```cpp
class Application {
public:
    explicit Application(const ApplicationOptions& options);

    // 设置/切换应用图标
    // setIcon 的路径解析与 ApplicationOptions.icon 一致：先查嵌入资源，再查可执行文件目录；找不到时使用系统默认图标并记录日志。
    void setIcon(std::string_view iconPath);

    // 创建窗口
    std::shared_ptr<WebWindow> createWindow(const WebWindowOptions& options);

    // 按 ID 获取窗口
    // ID 无效或对应窗口已销毁时返回 std::nullopt
    std::optional<std::shared_ptr<WebWindow>> getWindow(WindowId id) const;

    // 枚举所有窗口
    std::vector<std::shared_ptr<WebWindow>> windows() const;

    // 注册应用事件（生命周期、崩溃等）
    // 回调在 UI 线程执行，可通过 Event::preventDefault() 阻止默认行为
    // 对 window-all-closed，preventDefault 会阻止默认退出。
    Subscription on(std::string_view event,
                    std::function<void(Event&, const nlohmann::json&)> callback);

    // 为事件循环挂载任务。
    // 每轮事件循环触发一次，回调在 UI 线程执行。
    // 返回的 Subscription 可用于取消挂载；需取消时调用 unsubscribe()。
    // 用途：作为通用扩展点，将非 fd 驱动、无法直接接入平台主循环的事件源，
    //       周期性地接入 Whiz 的事件循环。
    // 注意：回调中不应执行耗时操作，否则会阻塞 UI 与 WebView。
    Subscription onEventLoopTick(std::function<void()> callback);

    // 退出应用
    void quit();

    // 运行事件循环，返回进程退出码，通常为 0；非 0 表示异常退出
    int run();

    // 应用路径
    std::string getPath(SysPath path) const;

    // 读取嵌入资源字节；path 为相对嵌入根目录的路径（如 "assets/icon.png"）。
    // 未找到对应资源时返回 std::nullopt；否则返回原始字节副本（std::vector<std::uint8_t>）。
    std::optional<std::vector<std::uint8_t>> readEmbeddedFile(std::string_view path) const;

    // 单实例锁
    bool requestSingleInstanceLock();
    void releaseSingleInstanceLock();
    bool hasSingleInstanceLock() const;
};
```

#### 7.3.1 应用生命周期事件

通过 `app.on(...)` 注册：

| 事件名 | 说明 | 可否 `preventDefault` |
|---|---|---|
| `ready` | 应用初始化完成 | 否 |
| `before-quit` | 退出前触发 | 是 |
| `will-quit` | 即将退出 | 是 |
| `quit` | 已退出 | 否 |
| `activate` | macOS 点击 Dock 图标 | 否 |
| `window-all-closed` | 所有窗口关闭 | 是（阻止默认退出；默认行为见下） |
| `second-instance` | 第二实例启动 | 否 |
| `js-error-crash` | JS 异常上报（渲染进程错误） | 否 |

- `Application::on` 的事件名仅支持表中列出的应用事件
- `app.on` 可在 `app.run()` 前后任意时刻调用。但 `ready` 事件在 `app.run()` 进入事件循环后很快触发，建议在 `app.run()` 之前注册。

**`window-all-closed` 默认行为与 `preventDefault()`：**

- 非 macOS：默认退出应用。
- macOS：默认不退出，保持应用活跃。
- 监听器可调用 `event.preventDefault()` 阻止本次默认退出。非 macOS 下阻止后应用保持活跃；macOS 下默认本就不退出，调用 `preventDefault()` 不改变结果。
- `preventDefault()` 只阻止由 `window-all-closed` 触发的默认退出，不影响后续显式 `app.quit()`，也不会跳过 `before-quit` / `will-quit` 流程。
- 若希望所有平台都默认不退出，可在监听器中统一 `preventDefault()`，再在业务需要时调用 `app.quit()`。

```cpp
// 所有平台都保持活跃，由业务决定何时退出
app.on("window-all-closed", [&](whiz::Event& e, const nlohmann::json&) {
    e.preventDefault();
});
```

**`before-quit` / `will-quit`：**

两者均可调用 `event.preventDefault()` 阻止退出。

**`second-instance` 事件参数：**

```json
{
  "argv": ["..."],
  "cwd": "/path",
  "windowId": 1
}
```

第二实例不会自动退出，由使用者决定后续行为（例如激活已有窗口）。标准做法是 `requestSingleInstanceLock()` 返回 `false` 后立即 `app.quit()`。

#### 7.3.2 应用路径 API

```cpp
enum class SysPath {
    UserData,
    Temp,
    Downloads,
    Documents,
    AppDir,
    Home,
    Cache,
    Logs
};

std::string Application::getPath(SysPath path) const;
```

JS 侧对应 `window.whiz.os.getPath(...)`，受 `permissions.os` 控制，且为异步 API。

`Application::getPath` 在 C++ 侧不受 `permissions.os` 影响；该权限仅控制 JS 侧 `window.whiz.os` 是否可用。

### 7.3.3 事件循环任务（`onEventLoopTick`）

`onEventLoopTick` 是 Whiz 提供的**通用事件循环扩展点**。它允许使用者在每一轮事件循环中执行一段轻量代码，用于桥接外部事件源或执行周期性检查。

```cpp
whiz::Application app(appOpts);

auto sub = app.onEventLoopTick([] {
    // 每轮事件循环执行一次
    // 回调在 UI 线程执行
});
```

**语义与约束：**

- **调用时机**：每轮事件循环执行一次，回调在 UI 线程。
- **注册时机**：可在 `app.run()` 之前或之后调用。在 `app.run()` 之前注册的任务，会在事件循环开始后立即参与调度。
- **取消方式**：通过返回的 `Subscription` 的 `unsubscribe()` 取消（析构不会自动取消）。可跨线程调用。行为与 `windowEventHandle` / `ipcHandle` 返回的 `Subscription` 一致。
- **执行顺序**：多个任务按注册顺序依次执行。若某个任务抛出异常，Whiz 会捕获并记录日志，不会中断其他任务。
- **重入禁止（硬约束）**：任务回调中**禁止**再次注册或取消 `onEventLoopTick` 任务。Whiz 会在遍历期间检测这一行为并触发断言失败，程序将直接退出。如需动态增删，请通过 `std::async` 或 `emit` 调度到下一轮。
  > 违反此约束导致的迭代器失效、任务丢失或其他异常行为，Whiz 不保证可恢复。请务必遵守。
- **耗时限制**：**任务回调中不应执行耗时操作。** 该回调在 UI 线程执行，任何阻塞都会直接冻结窗口和 WebView 的响应。需要耗时逻辑时，应投递到线程池（如 `std::async`），完成后通过 `emit` 回到 UI 线程。

**实现说明：**

Saucer v8.x 未提供 `loop::hook` 之类的直接接口，Whiz 内部通过 Saucer 的异步任务调度能力（`context->async()` 或 `loop->post()`）实现：在每次回调末尾重新投递下一轮任务，从而形成持续的事件循环任务链。用户无需关心这一实现细节。

任务存储与生命周期管理遵循以下规则：

- **有序存储**：任务使用 `std::vector` 存储，保持注册顺序，遍历时按注册顺序执行。
- **标记法取消**：每个任务项带有一个存活标志（`bool alive`）。`unsubscribe()` 不立即从 `std::vector` 中移除任务，而是将该标志置为 `false`；遍历时跳过 `alive == false` 的任务；真正从 `std::vector` 中移除（`erase`）统一放在遍历结束之后执行。
- **跨线程 `unsubscribe`**：`unsubscribe()` 可从任意线程调用，其行为与 `windowEventHandle` / `ipcHandle` 的 `Subscription` 保持一致——若回调正在执行，不会中断当前回调；当前回调执行完成后不再触发后续回调。跨线程调用会被调度到 UI 线程处理。
- **重入检测**：Whiz 内部维护一个 `iterating_` 标志，在遍历任务列表期间置位。若在此期间检测到注册或取消操作（无论来自哪个线程），Whiz 会记录日志并触发断言失败，程序直接退出。

**适用与不适用场景：**

| 场景 | 推荐做法 |
|---|---|
| 外部事件源能以 fd 形式接入平台主循环（如 Linux D-Bus fd、Windows 隐藏窗口消息、macOS `NSStatusItem`） | **优先走平台事件源机制**，不要用 `onEventLoopTick` |
| 周期性检查、状态轮询、轻量级定时任务 | 使用 `onEventLoopTick` |
| 耗时任务 | 投递到线程池，通过 `emit` 回 UI 线程 |
| 需要精确高频（< 16ms）调度 | 不推荐，应改用平台原生定时器或事件源 |

> **对 whiz-tray 等扩展库的说明**：系统托盘等外部事件源通常优先采用 fd 注册（Linux）、隐藏窗口消息（Windows）、`NSStatusItem`（macOS）等平台原生事件源机制接入主循环，`onEventLoopTick` **不是这类集成的必需品**，仅作为通用扩展点存在。它是给那些无法以 fd 形式接入平台主循环的事件源准备的兜底方案。

#### 7.3.4 读取嵌入资源

C++ 侧可用 `Application::readEmbeddedFile` 读取嵌入资源的原始字节，用于在运行时取回图片、字体等二进制文件（例如 `icon.png`）。

```cpp
std::optional<std::vector<std::uint8_t>> Application::readEmbeddedFile(std::string_view path) const;
```

- `path` 为相对嵌入根目录的路径（与 `loadFile` 的 `entry` 同一命名空间），如 `"assets/icon.png"`。
- 未找到对应资源时返回 `std::nullopt`。
- 命中时返回 `std::vector<std::uint8_t>`，是嵌入字节的副本，可安全地自行持有与修改。
- 该接口在 C++ 侧可用，不受 `permissions.fs` 等 JS 权限控制（权限仅约束 JS 侧 `window.whiz.fs`）。

### 7.4 WebWindow

```cpp
using WindowId = std::uint64_t; // 从 1 开始，0 表示无效

class WebWindow {
public:
    // 窗口 ID
    WindowId id() const;

    // 加载嵌入资源中的相对路径。会先规范化路径，再判断是否仍在嵌入根目录内；
    // ../ 在根目录内允许，规范化后跳出根的路径会被拒绝。
    // entry 为空时抛出 std::invalid_argument。
    void loadFile(std::string_view entry);

    // 加载远程 URL。loadURL 完全继承 Saucer 默认策略，Whiz 不做额外修改。如需限制协议或域名，请由使用者在 WebView 层自行处理。
    void loadURL(std::string_view url);

    // 全屏
    void setFullscreen(bool fullscreen);
    bool isFullscreen() const;

    // 尺寸
    void setSize(int width, int height);
    void setMinSize(int width, int height);
    void setResizable(bool resizable);
    std::pair<int, int> getSize() const;

    // 位置
    void setPosition(int x, int y);
    std::pair<int, int> getPosition() const;
    void center();

    // 窗口状态
    void maximize();
    void minimize();
    void restore();
    void close();
    void hide();
    void show();
    void focus();
    void setAlwaysOnTop(bool enabled);
    void setDecorated(bool enabled);       // 与 setWindowFrameState 等价（兼容旧接口）
    // frame=false 时移除标题栏/边框，但保留阴影与调整大小能力（decoration::partial）
    void setWindowFrameState(bool frame);
    bool hasWindowFrame() const;
    void setTitle(std::string_view title);
    std::string title() const;
    bool isMaximized() const;
    bool isMinimized() const;
    bool isVisible() const;

    // 窗口事件监听。回调在 UI 线程执行，可 preventDefault。
    Subscription windowEventHandle(
        std::string_view event,
        std::function<void(Event&, const nlohmann::json&)> callback);

    // IPC 请求处理。回调在线程池执行，返回 future 表示异步响应。
    // 标准用法见第 9 章：所有回调都应返回 std::future<nlohmann::json>。
    Subscription ipcHandle(
        std::string_view event,
        std::function<std::future<nlohmann::json>(const nlohmann::json&)> callback);

    // 主动向 JS 推送消息（线程安全）
    void emit(std::string_view event, const nlohmann::json& data);

    // 打开 DevTools（需权限 devtools），未启用 permissions.devtools 时调用将抛出异常
    void openDevTools();

    // ---- 对话框 ----

    // 打开文件选择对话框。返回选中的文件路径列表，取消时返回 std::nullopt。
    // 必须在 UI 线程调用；回调在 UI 线程执行。
    void showOpenDialog(const OpenDialogOptions& opts,
                        std::function<void(std::optional<std::vector<std::string>>)> callback);

    // 打开保存文件对话框。返回保存路径，取消时返回 std::nullopt。
    // 必须在 UI 线程调用；回调在 UI 线程执行。
    void showSaveDialog(const SaveDialogOptions& opts,
                        std::function<void(std::optional<std::string>)> callback);

    // 显示原生消息框。始终通过回调返回 MessageBoxResult。
    // 用户在标题栏关闭对话框时 response 为 -1；
    // 按 Esc 且未设置 cancelButtonIndex 时同样为 -1；
    // 按 Esc 且设置了 cancelButtonIndex 时返回该索引；
    // 点击 cancelButtonIndex 对应按钮时返回该按钮索引。
    // 必须在 UI 线程调用；回调在 UI 线程执行。
    void showMessageBox(const MessageBoxOptions& opts,
                        std::function<void(MessageBoxResult)> callback);
};
```

> **IPC 单 handler 语义**：每个事件名只能注册一个 handler。同一事件名重复注册会抛出 `std::logic_error`（异常信息包含事件名）。通过 `Subscription::unsubscribe()` 取消注册；取消后该事件名可再次注册。已取消（未注册）的事件名被 `invoke` 时会返回 `IPC_HANDLER_NOT_FOUND` 错误，而非静默挂起。

#### 7.4.1 窗口事件

通过 `ww->windowEventHandle(...)` 注册，回调在 UI 线程执行，返回 `Subscription` 可用于取消。

| 事件名 | 参数 | 可否 `preventDefault` |
|---|---|---|
| `close` | 无 | 是 |
| `closed` | 无 | 否 |
| `resize` | `{ width, height }` | 否 |
| `move` | `{ x, y }` | 否 |
| `focus` | 无 | 否 |
| `blur` | 无 | 否 |
| `minimize` | 无 | 否 |
| `maximize` | 无 | 否 |
| `restore` | 无 | 否 |
| `show` | 无 | 否 |
| `hide` | 无 | 否 |
| `sub-window-close` | `{ windowId }` | 否 |

### 7.5 父子窗口与多窗口协作

- `app.windows()` 返回 `std::vector<std::shared_ptr<WebWindow>>`，包含所有窗口。
- 通过 `WebWindowOptions.parentWebWindow` 指定父窗口（`std::weak_ptr<WebWindow>`）。
- **父窗口关闭时，子窗口跟随关闭；子窗口关闭不影响父窗口。**
- 父窗口可监听子窗口关闭事件 `sub-window-close`。**子窗口关闭时，无论主动关闭还是因父窗口关闭而跟随关闭，都会触发父窗口的 `sub-window-close` 事件。**
- 若子窗口创建时，发现传入的 `parentWebWindow` 弱指针已失效，子窗口按无父窗口处理，不跟随任何窗口关闭。

```cpp
parent->windowEventHandle("sub-window-close", [](whiz::Event&, const nlohmann::json& args) {
    auto childId = args["windowId"];
    // 处理子窗口关闭
});
```

---

## 8. JavaScript API (`window.whiz`)

所有功能挂在全局对象 `window.whiz` 下，且仅在 C++ 侧配置了对应权限时才可用。未配置权限的模块在 JS 环境中为 `undefined`。

若 `permissions.ipc = false`，则 `window.whiz.ipc` 未定义，访问结果为 `undefined`。

### 8.1 IPC

C++ 侧回调的标准写法见第 9 章。所有 ipcHandle 回调都应返回 ```std::future<nlohmann::json>```，避免阻塞线程池。

```js
// 向 C++ 发送请求。底层始终构造数组：[eventName, ...args]，至少包含事件名。
// C++ 侧 ipcHandle 回调收到的 args 是除事件名外的参数数组。
const result = await window.whiz.ipc.invoke("event-name", arg1, arg2);

// 无参数调用
const result2 = await window.whiz.ipc.invoke("event-name"); // C++ 收到 []

// 接收 C++ 主动推送的消息
const off = window.whiz.ipc.on("event-name", (data) => {
    console.log(data);
});

// 需要时取消监听；简单场景直接忽略返回值即可
// off();
```

> `invoke` 超时为 **默认不超时**，没有 `signal`，不可取消。  
> 所有参数都会进入参数数组，不会被识别为选项。  
> 需要自定义超时或取消时，使用 `createInvoker`。  
> `on` 返回一个取消函数（可选使用），不提供 `off` 和 `once`。

#### 8.1.1 自定义超时与取消：`createInvoker`

如果需要自定义超时，或希望支持取消，使用 `createInvoker` 创建一个绑定了选项的 `invoke` 函数：

```js
const controller = new AbortController();

const invoke = window.whiz.ipc.createInvoker({
    timeout: 5000,              // 可选，单位毫秒；不传则默认不超时
    signal: controller.signal   // 可选，不传则不可取消
});
// timeout 与 signal 同时触发时，谁先触发谁生效，错误码分别为 IPC_TIMEOUT / IPC_CANCELLED

try {
    const result = await invoke("slow-task", arg1, arg2);
} catch (err) {
    if (err.code === "IPC_TIMEOUT") { /* ... */ }
    if (err.code === "IPC_CANCELLED") { /* ... */ }
}

// 取消该 invoker 的所有未完成调用
controller.abort();
```

说明：

- `createInvoker` 的 options 只在创建时指定，不会与业务参数混淆。
- 返回的 `invoke` 与普通 `invoke` 调用方式一致：`invoke(event, ...args)`。
- `signal` 是共享的：一旦 `abort()`，该 invoker 的所有未完成调用都会 reject `IPC_CANCELLED`，之后的新调用也会立即 reject。
- 如果每次调用需要独立取消，创建多个 invoker，每个绑定自己的 `AbortController`。
- **超时或取消后，JS Promise 会 reject；C++ 侧 future 仍会执行到完成，但结果会被丢弃。** Whiz 不会在 future 完成后再尝试 resolve/reject 已 reject 的 Promise，也不会抛出未处理异常。
- Whiz 当前不提供 C++ 侧取消令牌；如需真正取消 C++ 任务，需由使用者在回调内自行实现取消逻辑。

#### 8.1.2 错误传递

当 C++ 侧回调抛出异常时，JS 的 `invoke` Promise 会 reject，错误对象格式：

```ts
{
  name: "WhizIpcError",
  code: string,
  message: string,
  stack?: string,
  details?: any
}
```

推荐错误码：

```txt
IPC_HANDLER_NOT_FOUND
IPC_HANDLER_EXCEPTION
IPC_INVALID_ARGUMENT
IPC_PERMISSION_DENIED
IPC_TIMEOUT
IPC_CANCELLED
IPC_INTERNAL
```

C++ 侧提供 `whiz::IpcError`，普通异常映射：

- `std::invalid_argument` → `IPC_INVALID_ARGUMENT`
- 其他未捕获异常 → `IPC_HANDLER_EXCEPTION`
- details 由 whiz::IpcError 自定义填充，普通异常时为空

### 8.2 OS（需 `permissions.os = true`）

所有 `os` 方法均为**异步**，返回 Promise。

```js
await window.whiz.os.platform(); // 'win32' | 'darwin' | 'linux'
await window.whiz.os.arch();     // 'x64' | 'arm64' | ...
await window.whiz.os.homedir();  // '/home/username/'

// 应用路径
await window.whiz.os.getPath("userData" | "temp" | "downloads" | "documents" | "appDir" | "home" | "cache" | "logs");
```

### 8.3 FS（需 `permissions.fs.read` / `permissions.fs.write`）

参考 Node.js 风格，通过**独立桥接**实现，不走 IPC。默认关闭，启用前请评估安全风险。Whiz 不对路径做白名单限制，路径访问安全由操作系统或容器工具负责。

```js
// 全量读取，返回 Promise<Uint8Array>
const data = await window.whiz.fs.readFile("/path/to/file");

// 全量写入
await window.whiz.fs.writeFile("/path/to/file", new Uint8Array([1, 2, 3]));

// 流式读取，返回 Promise<ReadableStream>（Web Streams 封装）
const readStream = await window.whiz.fs.createReadStream("/path/to/file", { highWaterMark: 1024 });

// 流式写入，返回 Promise<WritableStream>（Web Streams 封装）
const writeStream = await window.whiz.fs.createWriteStream("/path/to/file");

// 文件信息，返回 Promise<Stats>
// 当路径不存在时，返回的Stats对象中设置 exists 为 false，而 size 为 0、mtimeMs / mtimeISO 为空/0、type 为 'other'，只有 exists 可靠。
const stats = await window.whiz.fs.stat("/path/to/file");

// 判断路径（文件或目录等）是否存在，返回 Promise<boolean>。这是 Whiz 自有 API。
const exists = await window.whiz.fs.exists("/path/to/file");
```

**权限：**

- `permissions.fs.read` 控制读取，`permissions.fs.write` 控制写入。
- 不提供 `fsWatch` 相关 API。

**错误格式（统一为 IPC 风格）：**

```ts
{
  name: "WhizFsError",
  code: "FS_NOT_FOUND" | "FS_PERMISSION_DENIED" | "FS_IO_ERROR" | ...,
  message: string,
  stack?: string,
  details?: {
    path?: string,
    syscall?: string
  }
}
```

- `FS_NOT_FOUND` 由 `readFile` / `writeFile` 等“**路径必须存在**”的操作使用；stat / exists 不产生该错误码。

**二进制传输：**

内部优先使用 Saucer 桥接能力，若不可用则回退 base64，但 JS API 始终表现为 `Uint8Array`。大文件使用 base64 回退时会有约 33% 的体积膨胀，请优先确保 Saucer 桥接可用。

### 8.4 窗口控制（需 `permissions.window = true`）

所有方法返回 Promise。

```js
await window.whiz.window.setTitle("New Title");
await window.whiz.window.getSize();          // await 后得到 { width, height }
await window.whiz.window.setPosition(x, y);
await window.whiz.window.center();
await window.whiz.window.maximize();
await window.whiz.window.minimize();
await window.whiz.window.restore();
await window.whiz.window.close();
await window.whiz.window.hide();
await window.whiz.window.show();
await window.whiz.window.focus();
await window.whiz.window.setAlwaysOnTop(true);
await window.whiz.window.setDecorated(false);
await window.whiz.window.openDevTools();
await window.whiz.window.isMaximized();
await window.whiz.window.isMinimized();
await window.whiz.window.isVisible();
```

平台不支持的操作会抛出错误 / reject。

### 8.5 对话框（需 `permissions.dialog = true`）

所有方法返回 Promise。

#### 8.5.1 文件选择对话框

基于 Saucer 官方模块 `saucer/desktop` 的 File-Picker 实现，使用平台原生文件选择器。

```js
// 打开文件选择对话框
// 返回: { canceled: boolean, filePaths: string[] }
const result = await window.whiz.dialog.showOpenDialog({
    title: "选择文件",
    defaultPath: "/home/user",
    multiSelect: false,
    showHidden: false,
    directory: false,
    filters: [
        { name: "Images", extensions: ["png", "jpg", "jpeg"] },
        { name: "All Files", extensions: ["*"] }
    ]
});

if (!result.canceled) {
    console.log(result.filePaths); // ["/home/user/photo.png"]
}

// 保存文件对话框
// 返回: { canceled: boolean, filePath: string }
const saveResult = await window.whiz.dialog.showSaveDialog({
    title: "保存文件",
    defaultName: "document.txt",
    filters: [{ name: "Text", extensions: ["txt"] }]
});

if (!saveResult.canceled) {
    console.log(saveResult.filePath);
}
```

**参数说明：**

| 参数 | 类型 | 适用 | 说明 |
|---|---|---|---|
| `title` | `string` | open / save | 对话框标题 |
| `defaultPath` | `string` | open / save | 默认路径，仅支持文件系统路径，不支持嵌入资源相对路径 |
| `defaultName` | `string` | save | 默认文件名 |
| `multiSelect` | `boolean` | open | 是否允许多选 |
| `showHidden` | `boolean` | open | 是否显示隐藏文件 |
| `directory` | `boolean` | open | 是否选择目录而非文件 |
| `filters` | `FileFilter[]` | open / save | 文件类型过滤器 |

> **路径与文件名组合规则**：当 `defaultPath` 和 `defaultName` 同时提供时，`defaultPath` 作为目录，`defaultName` 作为该目录下的初始文件名。仅提供 `defaultPath` 时，若路径指向具体文件则作为初始文件名。

> **`directory: true` 与 `multiSelect` 的交互**：`directory: true` 时，`multiSelect` 的支持因平台而异。Windows / Linux 通常支持多选目录，macOS 的 `NSOpenPanel` 支持通过 `allowsMultipleSelection` 多选目录。

**返回值：**

| API | 返回值 | 取消/关闭时 |
|---|---|---|
| `showOpenDialog` | `{ canceled: boolean, filePaths: string[] }` | `{ canceled: true, filePaths: [] }` |
| `showSaveDialog` | `{ canceled: boolean, filePath: string }` | `{ canceled: true, filePath: "" }` |

**用户确认但未选择任何文件时，按平台原生行为处理**：部分平台返回 canceled: false, filePaths: []，部分平台返回 canceled: true, filePaths: []。如需区分“取消”与“空选”，**建议在业务层不依赖此差异**。

#### 8.5.2 原生消息框

基于 Saucer Native API 访问底层平台数据，调用平台原生消息框 API 实现。

```js
// 显示原生消息框
// 返回: { response: number, checkboxChecked: boolean }
const msgResult = await window.whiz.dialog.showMessageBox({
    type: "question",          // 图标类型，可选 "none" | "info" | "warning" | "error" | "question"，默认 "none"
    title: "确认操作",
    message: "确定要删除这个文件吗？",
    detail: "删除后无法恢复。",
    buttons: ["取消", "删除"],
    defaultButtonIndex: 1,
    cancelButtonIndex: 0,
    checkboxLabel: "不再提示",   // 可选
    checkboxChecked: false,     // 可选，复选框初始状态，默认 false
    modal: true                 // 可选，默认 true
});

console.log(msgResult.response);         // 用户点击的按钮索引
console.log(msgResult.checkboxChecked);  // 复选框状态
```

**参数说明：**

| 参数 | 类型 | 说明 |
|---|---|---|
| `type` | `"none" \| "info" \| "warning" \| "error" \| "question"` | 图标类型，默认"none" |
| `title` | `string` | 对话框标题 |
| `message` | `string` | 主体消息 |
| `detail` | `string` | 可选，详细信息 |
| `buttons` | `string[]` | 按钮文本列表，至少一个 |
| `defaultButtonIndex` | `number` | 默认按钮索引（回车触发），默认 `0` |
| `cancelButtonIndex` | `number` | 取消按钮索引（Esc 触发），-1 表示无取消按钮 |
| `checkboxLabel` | `string` | 可选，复选框文本；为空或未提供时不显示复选框，此时 `checkboxChecked` 被忽略 |
| `checkboxChecked` | `boolean` | 可选，复选框初始状态，默认 `false` |
| `modal` | `boolean` | 可选，是否模态，默认 `true` |

> **`type` 字符串映射**：`"none"` → `MessageBoxType::None`，`"info"` → `Info`，`"warning"` → `Warning`，`"error"` → `Error`，`"question"` → `Question`。传入其他字符串时 reject `DIALOG_INVALID_ARGUMENT`。

> **模态语义**：`modal = true` 时，对话框会阻止与创建它的窗口的交互；`modal = false` 时，对话框不阻止任何窗口交互，但仍归属于创建它的窗口，窗口关闭时对话框一并关闭；对话框不会独立于窗口存在。JS 侧 `modal` 默认为 `true`。

> **参数校验**：`buttons` 为空数组时会 reject `DIALOG_INVALID_ARGUMENT`，错误信息说明至少需要一个按钮。

> **`cancelButtonIndex` 与 `defaultButtonIndex` 的关系**：二者指向同一按钮时，行为由平台原生对话框决定；建议避免这种配置。

> **`checkboxChecked` 与 `checkboxLabel` 的关系**：`checkboxLabel` 为空或未提供时，`checkboxChecked` 被忽略，返回值中的 `checkboxChecked` 恒为 `false`。

**返回值：**

| 字段 | 类型 | 说明 |
|---|---|---|
| `response` | `number` | 用户点击的按钮索引（0 起始）；用户通过标题栏关闭对话框时为 `-1`；按 Esc 且未设置 `cancelButtonIndex` 时同样为 `-1`；按 Esc 且设置了 `cancelButtonIndex` 时返回该索引；点击取消按钮时返回该按钮索引 |
| `checkboxChecked` | `boolean` | 复选框最终状态；未启用复选框时为 `false` |

#### 8.5.3 平台差异说明

**showOpenDialog / showSaveDialog：**

- `defaultPath` 仅支持文件系统路径，不支持嵌入资源相对路径。
- `filters` 的具体呈现由平台原生控件决定，三平台的交互细节可能略有不同。
- `directory: true` 时，`filters` 在大多数平台被忽略。

**showMessageBox：**

- **Linux (GTK 4.10+)**：使用 `GtkAlertDialog`，支持标题、消息、详情和按钮。
- **Linux (GTK < 4.10)**：使用 `gtk_message_dialog_new`，`detail` 字段会被合并到 `message` 主体中显示。部分 GTK 4.x 早期版本可能将 `detail` 独立显示为次级文本。
- **Windows**：使用 `TaskDialogIndirect`（可用时）或 `MessageBoxW`。按钮文字长度建议不超过 100 字符。
- **macOS**：使用 `NSAlert`。`detail` 以副标题形式显示，按钮文字建议不超过 80 字符。
- `checkboxLabel` 在部分平台（如 GTK 4.10 以下回退路径、部分 Windows 对话框）不受支持，此时复选框不会显示，`checkboxChecked` 返回 `false`。
- `modal` 的具体实现由平台原生对话框的模态机制决定。

#### 8.5.4 错误格式

```ts
{
  name: "WhizDialogError",
  code: "DIALOG_PERMISSION_DENIED" | "DIALOG_PLATFORM_ERROR" | "DIALOG_INVALID_ARGUMENT" | "DIALOG_INTERNAL",
  message: string,
  stack?: string,
  details?: any
}
```

推荐错误码：

| 错误码 | 触发条件 |
|---|---|
| `DIALOG_PERMISSION_DENIED` | 未启用 `permissions.dialog` |
| `DIALOG_PLATFORM_ERROR` | 平台原生 API 调用失败 |
| `DIALOG_INVALID_ARGUMENT` | 参数非法（如 `buttons` 为空、`type` 字符串未知） |
| `DIALOG_INTERNAL` | 内部错误 |

> 正常取消（用户关闭对话框或点击取消按钮）不会 reject，而是通过返回值表达：`showOpenDialog` / `showSaveDialog` 返回 `{ canceled: true, ... }`；`showMessageBox` 返回的 `response` 字段按以下规则确定——通过标题栏或系统关闭方式关闭、且未设置 `cancelButtonIndex` 时为 `-1`；按 Esc 键时，设置了 `cancelButtonIndex` 则返回该索引，未设置则返回 `-1`；点击取消按钮时返回 `cancelButtonIndex`。

---

## 9. IPC 异步处理与数据转换

### 9.1 C++ 侧异步处理

`ipcHandle` 的回调在线程池执行，返回 `std::future<nlohmann::json>` 表示这是一个 IPC 请求响应。

```cpp
ww->ipcHandle("get-user", [](const nlohmann::json& args) -> std::future<nlohmann::json> {
    return std::async(std::launch::async, []() {
        nlohmann::json result;
        result["name"] = "Alice";
        result["age"] = 30;
        return result;
    });
});
```

### 9.2 结构体与 JSON 互转

推荐为自定义结构体提供 `to_json` / `from_json`：

```cpp
struct UserRequest {
    int id;
};

void from_json(const nlohmann::json& j, UserRequest& r) {
    j.at("id").get_to(r.id);
}

struct UserResponse {
    std::string name;
    int age;
};

void to_json(nlohmann::json& j, const UserResponse& r) {
    j = nlohmann::json{{"name", r.name}, {"age", r.age}};
}

ww->ipcHandle("get-user", [](const nlohmann::json& args) -> std::future<nlohmann::json> {
    // args 是 JS 传入的参数数组，例如 [{ "id": 1 }]
    UserRequest req = args.at(0).get<UserRequest>(); 
    return std::async(std::launch::async, [req]() {
        UserResponse resp{"Alice", 30};
        return nlohmann::json(resp);
    });
});
```

### 9.3 JS 侧调用

```js
const res = await window.whiz.ipc.invoke("get-user", { id: 1 });
console.log(res.name); // "Alice"
```

### 9.4 C++ 主动推送

```cpp
ww->emit("server-message", {{"text", "Hello from C++"}});
```

```js
window.whiz.ipc.on("server-message", (data) => {
    console.log(data.text);
});
```

---

## 10. TypeScript 全局声明 (`whiz.d.ts`)

```ts
declare global {
    interface Window {
        whiz: {
            ipc?: {
                invoke(event: string, ...args: any[]): Promise<any>;

                createInvoker(options?: {
                    timeout?: number;
                    signal?: AbortSignal;
                }): (event: string, ...args: any[]) => Promise<any>;

                on(event: string, callback: (data: any) => void): () => void;
            };
            os?: {
                platform(): Promise<'win32' | 'darwin' | 'linux'>;
                arch(): Promise<'x64' | 'arm64'>;
                homedir(): Promise<string>;
                getPath(path: 'userData' | 'temp' | 'downloads' | 'documents' | 'appDir' | 'home' | 'cache' | 'logs'): Promise<string>;
            };
            fs?: {
                readFile(path: string): Promise<Uint8Array>;
                writeFile(path: string, data: Uint8Array): Promise<void>;
                createReadStream(path: string, options?: any): Promise<ReadableStream<Uint8Array>>;
                createWriteStream(path: string, options?: any): Promise<WritableStream<Uint8Array>>;
                stat(path: string): Promise<Stats>;
                exists(path: string): Promise<boolean>;
            };
            window?: {
                setTitle(title: string): Promise<void>;
                getSize(): Promise<{ width: number; height: number }>;
                setPosition(x: number, y: number): Promise<void>;
                center(): Promise<void>;
                maximize(): Promise<void>;
                minimize(): Promise<void>;
                restore(): Promise<void>;
                close(): Promise<void>;
                hide(): Promise<void>;
                show(): Promise<void>;
                focus(): Promise<void>;
                setAlwaysOnTop(enabled: boolean): Promise<void>;
                setDecorated(enabled: boolean): Promise<void>;
                openDevTools(): Promise<void>;
                isMaximized(): Promise<boolean>;
                isMinimized(): Promise<boolean>;
                isVisible(): Promise<boolean>;
            };
            dialog?: {
                showOpenDialog(options?: {
                    title?: string;
                    defaultPath?: string;
                    multiSelect?: boolean;
                    showHidden?: boolean;
                    directory?: boolean;
                    filters?: Array<{ name: string; extensions: string[] }>;
                }): Promise<{ canceled: boolean; filePaths: string[] }>;

                showSaveDialog(options?: {
                    title?: string;
                    defaultPath?: string;
                    defaultName?: string;
                    filters?: Array<{ name: string; extensions: string[] }>;
                }): Promise<{ canceled: boolean; filePath: string }>;

                showMessageBox(options: {
                    type?: 'none' | 'info' | 'warning' | 'error' | 'question';
                    title?: string;
                    message?: string;
                    detail?: string;
                    buttons: string[];   // buttons 必填，至少一个
                    defaultButtonIndex?: number;
                    cancelButtonIndex?: number;
                    checkboxLabel?: string;
                    checkboxChecked?: boolean;
                    modal?: boolean;
                }): Promise<{ response: number; checkboxChecked: boolean }>;
            };
        };
    }
}

interface Stats {
    size: number;
    mtimeMs: number;
    mtimeISO: string;
    exists: boolean; 
    isFile: boolean;
    isDirectory: boolean;
    type: 'file' | 'directory' | 'symlink' | 'other';
    // ... 其他 Node.js 风格字段，均为可序列化普通字段
}

export {};
```

---

## 11. 调试与日志

- **DevTools**：C++ 侧 `ww->openDevTools()`，JS 侧 `window.whiz.window.openDevTools()`；均需在 `ApplicationOptions.permissions.devtools` 中启用（JS 侧同时受 `permissions.window` 控制）。未启用时 C++ 抛出异常、JS Promise reject。Release 下不做自动空操作，由使用者自行控制。
- **统一日志**：C++ 侧日志输出到控制台；若设置了 `ApplicationOptions.logFile`，同时写入文件。相对路径相对于可执行文件目录，建议使用 `getPath` 拼接绝对路径。
- **logFile 父目录不存在时**，自动循环创建多级目录，创建失败则记录警告并仅输出到控制台
- **JS 日志转发**：当 `permissions.log = true` 时，JS 的 `console.log/warn/error` 会转发到 C++ 日志。
- **崩溃捕获 (仅限JS异常)**：JS 异常上报到 C++，通过 `app.on("js-error-crash", ...)` 监听。该事件**仅覆盖渲染进程内的 JS 异常**，例如未捕获的 JS 异常、Promise rejection 等。

    **该事件不覆盖原生崩溃**：例如 C++ 未捕获异常、段错误（SIGSEGV）、空指针访问、栈溢出、`std::terminate`、操作系统级信号崩溃等，均不会触发 `js-error-crash`。
    
    ```cpp
    app.on("js-error-crash", [](whiz::Event&, const nlohmann::json& error) {
        // 记录错误信息
    });
    ```

- **对话框诊断**：`showMessageBox` 在各平台的实现路径不同（详见 8.5.3），Whiz 会在初始化时记录当前使用的原生 API、GTK 版本（Linux），以及当前平台是否支持 `checkboxLabel`，便于排查外观/行为差异。
- **事件循环任务诊断**：`onEventLoopTick` 任务数量会随日志级别输出；任务回调中抛出的异常会被捕获并记录，便于定位接入的外部事件源问题。

---

## 12. 与 Electron 的对照

| Electron | Whiz | 说明 |
|---|---|---|
| `BrowserWindow` | `WebWindow` | 窗口 + WebView 聚合 |
| `loadURL(url)` | `loadURL(url)` | 直接对应，继承 Saucer 默认配置 |
| `loadFile(path)` | `loadFile(entry)` | 入口为嵌入资源中的相对路径，禁止 `../` 跳出根 |
| `setFullScreen(bool)` | `setFullscreen(bool)` | 直接对应 |
| `ipcMain.handle` | `ww->ipcHandle(event, cb)` | C++/JS 互操作，异步 JSON，回调返回 `std::future<json>` |
| `webContents.send` | `ww->emit` | C++ 主动推送 JS |
| `app.quit()` | `app.quit()` | 直接对应 |
| `asar` 打包 | `whiz_embed` | 资源嵌入二进制 |
| 前端构建 | 使用者自选 | Whiz 不介入 |
| `fs` 模块 | `window.whiz.fs` | 独立桥接，需显式开启权限，无路径白名单 |
| `os` 模块 | `window.whiz.os` | 需显式开启权限，含 `getPath`，全部异步 |
| 窗口控制 | `window.whiz.window` | 需 `permissions.window` |
| `dialog.showOpenDialog` | `window.whiz.dialog.showOpenDialog` | 基于 `saucer/desktop` File-Picker，需 `permissions.dialog` |
| `dialog.showSaveDialog` | `window.whiz.dialog.showSaveDialog` | 同上 |
| `dialog.showMessageBox` | `window.whiz.dialog.showMessageBox` | 基于 Saucer Native API，平台原生实现 |
| 单实例锁 | `app.requestSingleInstanceLock()` | 直接对应，返回 false 应立即 `app.quit()` |
| `app.on('window-all-closed', ...)` | `app.on("window-all-closed", ...)` + `Event::preventDefault()` | Whiz 保留平台默认；可用 `preventDefault()` 阻止默认退出。Electron 无 `preventDefault`，通常由监听器条件调用 `app.quit()`。 |

> Whiz 额外提供 `ipc.createInvoker` 用于自定义超时与取消，Electron 对应能力需通过第三方或自行实现。

> Whiz 额外提供 `Application::onEventLoopTick` 作为事件循环扩展点；Electron 无直接对应能力，通常需在 `app.whenReady()` 后自行启动定时器或监听外部事件源。

---

## 13. 目录结构

```txt
whiz/
├── CMakeLists.txt
├── include/
│   └── whiz/
│       ├── application.hpp
│       ├── web_window.hpp
│       ├── options.hpp
│       ├── dialog.hpp          # 对话框选项结构体
│       ├── subscription.hpp
│       ├── event.hpp
│       └── ipc_error.hpp
├── src/
│   ├── application.cpp
│   ├── web_window.cpp
│   ├── js_bridge.cpp
│   ├── fs_bridge.cpp
│   └── dialog_bridge.cpp       # 对话框实现
├── web/  # 默认示例前端，使用者需在CMake中用whiz_embed指定前端根目录
│   └── index.html
├── types/
│   └── whiz.d.ts  # TypeScript 全局声明
└── docs/
    └── README.md
```

---

## 14. 设计边界

**Whiz 提供：**

- 窗口与 WebView 生命周期管理
- C++/JS 双向异步调用（JSON）
- 前端资源嵌入
- 全局事件分发
- 事件循环扩展点（`onEventLoopTick`），用于接入外部事件源
- 常用桌面能力：文件系统、系统信息、应用路径、日志、崩溃捕获（均需显式授权，IPC 除外）
- 原生文件选择对话框（打开 / 保存），基于 `saucer/desktop` File-Picker
- 原生消息框对话框，基于 Saucer Native API
- 窗口控制、单实例锁
- 应用退出默认行为的事件级控制（`window-all-closed` 可 `preventDefault`）

**Whiz 不提供：**

- 前端构建流程（由使用者自行使用前端工具构建，如：`npm run build`）
- 本地开发服务器（使用者可用 Vite 等工具的 dev server）
- 自动更新、崩溃上报服务、打包安装器
- `fs.watch`
- 文件系统路径白名单（由系统或容器工具负责）
- C++ 侧 IPC 取消令牌（如需真正取消 C++ 任务，由使用者自行实现）
- 自定义 HTML 对话框渲染（由前端框架负责）
- 平台原生事件源（如 fd 监听、隐藏窗口消息）的直接封装；如外部事件源能以 fd 形式接入平台主循环，应由对应扩展库自行完成，Whiz 仅提供 `onEventLoopTick` 作为通用兜底

---

## 15. 安全说明

- `window.whiz.fs` 参考 Node.js，**默认关闭**。仅当 `permissions.fs.read` / `permissions.fs.write` 为 `true` 时才可访问。Whiz 不对路径做白名单限制，路径访问安全由操作系统或容器工具负责。
- `window.whiz.os` 会泄露系统信息，默认关闭。`getPath` 同样受此权限控制，且为异步 API。
- `window.whiz.window` 窗口控制默认关闭，需 `permissions.window = true`。
- `window.whiz.dialog` 默认关闭，需 `permissions.dialog = true`。文件选择对话框会将用户选择的完整文件系统路径暴露给 JS 环境。`showMessageBox` 不涉及文件系统路径，但仍是原生 UI 调用，受同一权限控制。
- `showMessageBox` 通过 Saucer Native API 调用平台原生实现，不经过 WebView 渲染，因此不受 `loadURL` 安全策略的约束。
- `DevTools` 默认关闭，生产环境请勿随意开启。权限关闭时，C++ 调用 `ww->openDevTools()` 会抛出异常、JS 调用 `window.whiz.window.openDevTools()` 会 reject，不会被静默忽略。
- 图标路径解析会同时尝试嵌入资源和可执行文件目录，找不到时使用系统默认图标并记录日志。
- IPC 请求回调（`ipcHandle`）在线程池中执行，注意线程安全，不要直接操作 UI。
- 窗口事件回调（`windowEventHandle`）在 UI 线程执行。
- 事件循环任务回调（`onEventLoopTick`）在 UI 线程执行，**不得执行耗时操作**，否则会冻结 UI 与 WebView；耗时任务应投递到线程池，完成后通过 `emit` 回到 UI 线程。
- 对话框方法（`showOpenDialog` / `showSaveDialog` / `showMessageBox`）**必须在 UI 线程调用**；违反时会被拒绝并记录警告。
- `emit` 线程安全，但消息投递到 UI 线程执行，存在微小延迟。
- `loadFile` 会规范化路径并禁止 `../` 跳出嵌入根目录。
- `loadURL` 默认继承 Saucer 的默认配置，Whiz 不做额外修改。
- `ipc.createInvoker` 的 `signal` 是共享的：`abort()` 会影响该 invoker 的所有未完成调用与后续调用。如需独立取消，请为每次调用创建独立 invoker。
- JS 侧超时或取消后，C++ 侧 future 仍会执行到完成，结果被丢弃；Whiz 不提供 C++ 侧取消令牌，如需真正取消，请由使用者在回调内自行实现。
- Linux 上 GTK 版本低于 4.10 时，`showMessageBox` 会回退到旧版 API。如果应用对对话框外观有严格要求，请在使用者文档中明确标注 GTK 4.10+ 为推荐版本。
- 若在 `window-all-closed` 中阻止默认退出，应用可能继续驻留，需自行管理退出时机与单实例锁释放。

---

## 附录 A：v3.4.2 变更摘要

- **新增** `Application::onEventLoopTick(std::function<void()> callback)`：为事件循环挂载任务，回调在 UI 线程执行，每轮事件循环触发一次。返回 `Subscription`，可用于取消挂载。
- **定位**：`onEventLoopTick` 是通用事件循环扩展点。系统托盘等外部事件源应优先采用平台原生事件源机制（Linux D-Bus fd、Windows 隐藏窗口消息、macOS `NSStatusItem`），`onEventLoopTick` 不是这类集成的必需品。
- **实现**：基于 Saucer v8 的异步任务调度能力（`context->async()` / `loop->post()`）递归投递实现。
- **实现约定**：`onEventLoopTick` 任务使用 `std::vector` 有序存储；取消采用标记法（`alive` 标志 + 遍历结束后统一 `erase`）；`unsubscribe` 跨线程行为与 `windowEventHandle` / `ipcHandle` 一致；遍历期间禁止注册/取消，违反会触发断言失败并退出程序。
- **文档**：在 7.1 线程模型、7.3 Application、11 调试与日志、12 与 Electron 对照、14 设计边界、15 安全说明等章节同步更新。
- **变更**：`window-all-closed` 事件支持 `Event::preventDefault()` 阻止默认退出。非 macOS 下阻止后应用保持活跃；macOS 默认不退出，`preventDefault()` 无额外效果。文档同步更新 7.2.2、7.3、7.3.1、12、14。