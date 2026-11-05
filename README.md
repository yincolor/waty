# waty

**English** | [简体中文](./README.zh.md)

A lightweight desktop application framework based on Saucer + C++23 + CMake, providing an Electron-like development experience: organize windows, WebView, and IPC in C++; frontend build artifacts can be embedded into the binary to enable single-file distribution.

## Features

- Window / WebView / IPC abstractions that are close to Electron's development model
- Frontend resource embedding (`waty_embed`), with the entry HTML determined by the C++ side
- Common desktop capabilities: file system, system information, path utilities, logging, crash capture, native dialogs, multi-database SQLite, system notifications, system tray
- Explicit permissions: capabilities exposed to JS are disabled by default (basic IPC is enabled by default)
- Usable as a CMake library and integrable via `FetchContent`

## Environment Requirements

- C++23 compiler (GCC 13+ / Clang 16+ / MSVC 19.35+)
- CMake 3.25+
- Linux development packages: `libgtk-4-dev`, `libadwaita-1-dev`, `libwebkitgtk-6.0-dev`, `libjson-glib-dev`, `libdbus-1-dev`, `libglib2.0-dev`, `libgdk-pixbuf-2.0-dev`
- Windows: WebView2 Runtime
- macOS: built-in Cocoa / WKWebView
- SQLite 3.49.1 (automatically fetched via FetchContent; CI must be able to access `github.com` and `sqlite.org`)

## Permissions Overview

JS-side capabilities are disabled by default (except IPC) and must be explicitly enabled in `ApplicationOptions::permissions`:

| Permission | Default | Available when enabled |
|---|---|---|
| `permissions.ipc` | `true` | `window.waty.ipc` |
| `permissions.os` | `false` | `window.waty.os` (`platform` / `arch`) |
| `permissions.path` | `false` | `window.waty.path` (synchronous path utilities / `getPath` / `resolve` / `cwd`) |
| `permissions.window` | `false` | `window.waty.window` (window control, DevTools) |
| `permissions.devtools` | `false` | `window.waty.window.openDevTools()` |
| `permissions.log` | `false` | JS `console.*` forwarded to C++ logs |
| `permissions.dialog` | `false` | `window.waty.dialog` |
| `permissions.fs.read` / `.write` | `false` | `window.waty.fs` |
| `permissions.sqlite3` | `false` | `window.waty.sqlite3` |
| `permissions.notify` | `false` | `window.waty.notify` |

## Building the Library

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The artifact is `libwaty.a`.

## Minimal Example

`main.cpp`:

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
    window->loadFile("index.html");   // Load from embedded resources
    window->show();

    return app.run();
}
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_app LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(waty
    GIT_REPOSITORY https://github.com/<you>/waty.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(waty)

add_executable(my_app main.cpp)

# waty_embed is available directly after FetchContent_MakeAvailable(waty)
waty_embed("${CMAKE_CURRENT_SOURCE_DIR}/web")

target_link_libraries(my_app PRIVATE waty::waty waty::embedded)
```

In your project, prepare `web/index.html` (the `web/` directory in this repository provides a reference example page), then:

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
./build/my_app
```

## Reading Embedded Resources

The C++ side can use `Application::readEmbeddedFile` to read the raw bytes of embedded resources (including images, fonts, and other binary files):

```cpp
if (auto bytes = app.readEmbeddedFile("assets/icon.png"))
{
    // *bytes is a std::vector<std::uint8_t>, a copy of the raw bytes
}
else
{
    // No matching embedded resource was found
}
```

`path` is relative to the embedded root (the same namespace as the `entry` passed to `loadFile`); it returns `std::nullopt` if not found.

## Path Module

Path functionality lives in the standalone header `<waty/paths.hpp>` and is no longer attached to `Application`:

```cpp
#include <waty/paths.hpp>

auto logs = waty::path::getPath(waty::SysPath::Logs, app_opts);
auto file = waty::path::join(logs, "app.log");
```

- Uses `std::filesystem` lexical semantics: `join("/a", "/b") == "/b"`, `join("a", "") == "a/"`, and empty results are normalized to `"."`.
- `normalize` / `join` / `resolve` / `basename` / `dirname` / `extname` / `isAbsolute` / `cwd` are all available.
- `getPath` requires explicit `organizationName` / `appName` when app-level paths are needed; you can also pass `ApplicationOptions`.
- Path functions reject URL schemes and throw `waty::PathError` on failure (`PATH_INVALID_ARGUMENT` / `PATH_NOT_AVAILABLE`).
- JS-side `window.waty.path` is a synchronous module and requires `permissions.path = true`; `path.cwd()` / `path.resolve()` use a cwd cache refreshed on every event loop iteration.
- v0.1.0 has removed `Application::getPath`, `os.homedir()`, `os.getPath()`, and the `SysPath::UserData` / `AppDir` aliases.

## IPC

The C++ side responds to the frontend's `window.waty.ipc.invoke(event, ...)` via `WebWindow::ipcHandle`:

- **Only one handler can be registered per event name.**
- **Registering the same event name twice throws `std::logic_error`** (the exception message includes the event name).
- **Unregister via the returned `Subscription::unsubscribe()`**; after unregistering, the event name can be registered again.
- Callbacks run on a thread pool and return `std::future<nlohmann::json>` for asynchronous responses.

## Sqlite3

`window.waty.sqlite3` provides multi-database SQLite access. It is disabled by default and requires explicitly enabling the permission:

```cpp
app_opts.permissions.sqlite3 = true;  // default false

// Optional: adjust the maximum number of simultaneously open databases (1..1024, default 64)
app_opts.sqlite3.maxOpenDatabases = 64;
```

Rules for `openDatabase(dbName, version)`:

- `dbName` matches `^[A-Za-z0-9_]{1,64}$` and is case-insensitive (internally normalized to lowercase).
- `version` matches `^\d{1,3}\.\d{1,3}\.\d{1,3}$` (for example, `0.0.1`). Different `version` values are independent databases; Waty does not perform schema migration.
- The same `(dbName, version)` opens only one connection across the whole application; the file is located at `<AppData>/sqlite3/db/<dbName>@<version>.db`.
- **Connections are shared across the application, while handles are accounted per window**: when multiple windows open the same database, they share the same connection; a window's `close()` only unregisters its own handle (subsequent operations from that window report `SQLITE3_NOT_OPEN`), and other windows continue to work normally. The connection is actually closed only when the last window calls `close` or when the window is closed/destroyed.
- A window that has never opened the database cannot operate on it directly even if it obtains the `dbId` (`SQLITE3_NOT_OPEN`).

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

- Each database uses an independent connection and worker thread; databases do not block one another; operations on the same database are strictly serialized (including across windows).
- Repeated `openDatabase` calls in the same window are idempotent (still one handle); a single `close()` unregisters that window's holding.
- `execute` / `query` accept only a single statement (multiple statements -> `SQLITE3_MULTI_STATEMENT`; use `transaction` instead); `execute` returns `{ changes, lastInsertRowid }`.
- `params` only supports named parameter objects (`:name` / `@name` / `$name`); arrays/objects passed as params are written as JSON text, and BLOB query results are returned as base64.
- Error objects are `{ name: "WatySqlite3Error", code, message }`; error codes include `SQLITE3_INVALID_ARGUMENT`, `SQLITE3_PERMISSION_DENIED`, `SQLITE3_TOO_MANY_OPEN`, `SQLITE3_OPEN_FAILED`, `SQLITE3_NOT_OPEN`, `SQLITE3_EMPTY_SQL`, `SQLITE3_MULTI_STATEMENT`, `SQLITE3_EXEC_ERROR`, etc. See `DESIGN.md` 8.6.4 for the complete list.
- Defaults are `journal_mode=WAL` and `busy_timeout=5000`; `foreign_keys` remains at SQLite's default off. Execute `PRAGMA foreign_keys=ON` when needed.

## System Notifications (Notify)

`window.waty.notify` provides system notifications (Windows Toast / macOS notifications / Linux D-Bus). It is disabled by default and requires explicitly enabling the permission:

```cpp
app_opts.permissions.notify = true;  // default false
```

```js
// Single callback: receives action / close / error for this notification
const { ok } = await window.waty.notify.send(
    {
        title: "Download complete",
        body: "movie.mp4 has been saved",
        timeout: 5000,                                  // milliseconds, optional
        closeOnAction: true,                            // default true: close after click and ignore subsequent actions
        actions: [{ id: "open", label: "Open" }]        // optional, at most 3
    },
    (event) => {
        switch (event.event) {
            case "action": console.log(event.actionId, event.label); break;
            case "close":  console.log(event.reason, event.reasonCode); break;
            case "error":  console.error(event.code, event.message); break;
        }
    }
);

// Structured handlers: handle each event type separately
await window.waty.notify.send(
    { title: "Download complete", actions: [{ id: "open", label: "Open" }] },
    {
        onAction(event) { console.log(event.actionId); },
        onClose(event)  { console.log(event.reason); },
        onError(event)  { console.error(event.code, event.message); }
    }
);

const status = await window.waty.notify.hasPermission();  // granted / denied / ...
await window.waty.notify.requestPermission();

// Compatible with legacy global events; per-send callbacks are the recommended approach
waty.ipc.on("notify-action", ({ id }) => { /* by default only the first action */ });
waty.ipc.on("notify-closed", ({ reason }) => { /* 4=action */ });
waty.ipc.on("notify-error", ({ code, message }) => {});
```

Event objects contain `callbackId`, `title`, `tag`, and: for `action`, `actionId` / `label`; for `close`, `reason` / `reasonCode`; for `error`, `code` / `message`. By default, clicking a button closes the notification immediately, `close.reason` is `"action"`, and subsequent clicks will not trigger again; callbacks are automatically removed after `close` / `error`.

The C++ side is also available via `<waty/notify.hpp>` (`waty::notify::send`, etc.); see [DESIGN.md](DESIGN.md) 7.6.

## System Tray (Tray)

The system tray provides only a C++ API (no JS bridge yet). Header `<waty/tray.hpp>`, namespace `waty::tray`, already compiled into the `waty` library:

```cpp
#include <waty/tray.hpp>

waty::tray::TrayContext context;
context.initialize();

auto tray = *waty::tray::TrayIcon::create();   // returns TrayError on failure
if (auto icon = waty::tray::Icon::from_file("assets/tray.png")) {
    tray->set_icon(*icon);
}
tray->show();
```

> Linux has a full implementation (SNI D-Bus); the Windows / macOS backends are currently placeholders and return `PlatformNotSupported`. See [DESIGN.md](DESIGN.md) 7.7.

## Network Notes

Dependencies are downloaded directly from GitHub by default. When the network is restricted, configure a proxy on your machine; the CMake scripts do not hardcode mirrors or offline caches:

```bash
export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890
git config --global http.proxy http://127.0.0.1:7890
git config --global https.proxy http://127.0.0.1:7890
```

## Testing

SQLite unit tests are not built by default. When enabled, run them via CTest (they do not depend on WebView / GTK):

```bash
cmake -B build -DWATY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests cover parameter validation, open idempotency/concurrency, open limit, window-level close and release, WAL, transaction rollback, BLOB/base64, large integers, multi-database isolation, concurrent writes, and the `SQLITE_THREADSAFE=1` assertion.

## Documentation

The full design document is in [DESIGN.md](DESIGN.md) (Sqlite3 see 8.6, notifications see 7.6 / 8.7, tray see 7.7, configuration see 6.2 / 6.2.1, thread model see 7.1).

## License

This project is licensed under the [GNU Affero General Public License v3.0 (AGPL-3.0)](https://www.gnu.org/licenses/agpl-3.0.html).