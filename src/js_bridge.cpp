// JS 桥实现（见 DESIGN.md §8）。
//
// 架构：
//   C++ 在窗口 realize 时注入一段自执行的 JS（k_bridge_script），
//   前端通过 window.waty.* 调用；JS 侧把请求序列化为 JSON 字符串，
//   经 window.saucer.internal.message() 发给 C++；C++ 在 handle_js_message
//   中按 "__waty" 通道分发，处理完后用 execute() 回调 __watyResolve/__watyReject。
//
// 消息包络（JS → C++）：
//   { "__waty": "ipc"|"os"|"path"|"window"|"fs"|"dialog"|"sqlite3"|"notify"
//                |"jserror"|"console",
//     "id": <number>,          // 仅请求类通道；C++ 原样回传
//     "op": "<操作名>",       // 通道内子操作
//     ...通道自定义字段 }     // args / path / options / dbId / ...
//
// 响应（C++ → JS）：
//   成功：window.__watyResolve(id, result)
//   失败：window.__watyReject(id, { name, code, message, details? })
//   推送：window.__watyEmit(event, data)（对应 JS 侧 waty.ipc.on）
//
// 权限注入：脚本中的 @@IPC@@ / @@OS@@ / ... 由 build_js_script() 替换为 true/false。
// 未启用的模块整段不会出现在页面里；C++ 侧各桥入口还会再做一次权限检查（纵深防御）。
//
// JS API 一览（需对应权限）：
//   waty.ipc.invoke(event, ...args) / waty.ipc.createInvoker({timeout,signal}) / waty.ipc.on(event, cb)
//   waty.os.platform() / waty.os.arch()
//   waty.path.{sep,delimiter,cwd,normalize,join,resolve,basename,dirname,extname,isAbsolute,getPath}
//   waty.window.{setTitle,getSize,setPosition,center,maximize,minimize,restore,close,hide,show,
//                focus,setAlwaysOnTop,setDecorated,openDevTools,isMaximized,isMinimized,isVisible}
//   waty.fs.{readFile,stat,exists,createReadStream} / {writeFile,createWriteStream}
//   waty.dialog.{showOpenDialog,showSaveDialog,showMessageBox}
//   waty.sqlite3.openDatabase(dbName, version) → { dbId, execute, query, transaction, close }
//   waty.notify.{send,hasPermission,requestPermission}

#include <waty/web_window.hpp>

#include "detail.hpp"

#include <saucer/webview.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace waty::detail
{
    namespace
    {
        // JS 桥脚本模板。@@XXX@@ 为权限占位符（"true"/"false"）。
        // 脚本幂等：window.__watyInit 为真时直接返回，避免重复注入破坏 pending 表。
        const char *k_bridge_script = R"js((function() {
    // =====================================================================
    // waty 浏览器端桥接层
    // ---------------------------------------------------------------------
    // 本脚本由 C++ 在创建 WebView 时注入，对页面暴露 window.waty。
    // 约定：
    //   * 所有跨进程调用返回 Promise；失败时 reject 一个 {name, code, message} 对象；
    //   * C++ 端用 __watySeq 自增 id 匹配请求/响应，页面不应直接使用 __waty*；
    //   * 模块是否出现由 C++ 注入的权限占位符决定（未启用的模块为 undefined）。
    // =====================================================================
    if (window.__watyInit) return;
    window.__watyInit = true;
    window.__watySeq = 0;
    window.__watyPending = {};
    window.waty = window.waty || {};
    window.waty.__listeners = {};

    window.__watyResolve = (id, value) => {
        const p = window.__watyPending[id];
        if (p) { delete window.__watyPending[id]; p.resolve(value); }
    };
    window.__watyReject = (id, error) => {
        const p = window.__watyPending[id];
        if (p) { delete window.__watyPending[id]; p.reject(error); }
    };
    window.__watyEmit = (event, data) => {
        const cbs = window.waty.__listeners[event];
        if (cbs) cbs.slice().forEach(cb => { try { cb(data); } catch (e) {} });
    };

    // -- 二进制传输：必须走 base64 ----------------------------------------
    // JSON 无法直接携带 ArrayBuffer/TypedArray，fs / sqlite3 BLOB 均以 base64 字符串
    // 在桥上来回；页面侧 readFile 会再转回 Uint8Array。
    const b64FromBytes = (bytes) => {
        let binary = "";
        for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
        return btoa(binary);
    };
    const bytesFromB64 = (b64) => {
        const binary = atob(b64);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
        return bytes;
    };

    // -- 通用请求：channel 对应 C++ 的 handle_js_message 分支 ---------------
    // 消息格式：{ __waty: channel, id, op?, ...extra }。
    const request = (channel, op, extra) => {
        return new Promise((resolve, reject) => {
            const id = ++window.__watySeq;
            window.__watyPending[id] = { resolve, reject };
            const msg = Object.assign({ __waty: channel, id }, extra || {});
            if (op !== undefined && op !== null) msg.op = op;
            window.saucer.internal.message(JSON.stringify(msg));
        });
    };

    // -- IPC 模块（默认开启） --------------------------------------------
    // invoke 把参数列表透传给 C++ 的 ipcHandle(event) 回调；
    // createInvoker 额外支持 timeout 与 AbortSignal（仅影响前端等待，不取消 C++ 侧执行）。
    if (@@IPC@@) {
        const makeInvoke = (options) => (event, ...args) => {
            return new Promise((resolve, reject) => {
                const id = ++window.__watySeq;
                window.__watyPending[id] = { resolve, reject };
                if (options && options.timeout != null) {
                    setTimeout(() => {
                        if (window.__watyPending[id]) {
                            delete window.__watyPending[id];
                            reject({ name: "WatyIpcError", code: "IPC_TIMEOUT", message: "IPC request timed out" });
                        }
                    }, options.timeout);
                }
                if (options && options.signal) {
                    const abort = () => {
                        if (window.__watyPending[id]) {
                            delete window.__watyPending[id];
                            reject({ name: "WatyIpcError", code: "IPC_CANCELLED", message: "IPC request cancelled" });
                        }
                    };
                    if (options.signal.aborted) { abort(); return; }
                    options.signal.addEventListener("abort", abort, { once: true });
                }
                window.saucer.internal.message(JSON.stringify({ __waty: "ipc", id, event, args }));
            });
        };
        window.waty.ipc = {
            invoke: makeInvoke(),
            createInvoker: (options) => makeInvoker(options),
            on: (event, callback) => {
                if (!window.waty.__listeners[event]) window.waty.__listeners[event] = [];
                window.waty.__listeners[event].push(callback);
                return () => {
                    const list = window.waty.__listeners[event];
                    if (list) {
                        const i = list.indexOf(callback);
                        if (i >= 0) list.splice(i, 1);
                    }
                };
            }
        };
    }

    // -- OS 模块：只返回平台/架构字符串，不暴露任意命令执行 -----------------
    if (@@OS@@) {
        window.waty.os = {
            platform: () => request("os", "platform"),
            arch: () => request("os", "arch")
        };
    }

    // -- PATH 模块：与 C++ waty::path 保持相同的词法语义 --------------------
    // @@PATH_DATA@@ 是 C++ 侧预取的路径表 + cwd 快照（JSON），避免每次调用跨桥。
    // cwd 由 C++ 每轮事件循环轮询并通过 __watyPathSetCwd 推送更新。
    // 注意：JS 实现按正斜杠/反斜杠平台分支，行为需与 src/paths.cpp 对齐。
    if (@@PATH@@) {
        const __watyPathData = @@PATH_DATA@@;
        const __watyPathSep = @@PATH_SEP@@;
        const __watyPathDelimiter = @@PATH_DELIMITER@@;
        const __watyPathIsWin = __watyPathSep === "\\";

        const __watyPathError = (code, message) => {
            const error = new Error(message);
            error.name = "WatyPathError";
            error.code = code;
            return error;
        };

        const __watyPathAssert = (value, name) => {
            if (typeof value !== "string") throw new TypeError(name + " 必须是字符串");
            if (/^[A-Za-z][A-Za-z0-9+.-]+:\/\//.test(value)) {
                throw __watyPathError("PATH_INVALID_ARGUMENT", name + " 必须是文件系统路径，不能是 URL: " + value);
            }
            return value;
        };

        const __watyPathIsSep = (ch) => __watyPathIsWin ? (ch === "/" || ch === "\\") : (ch === "/");

        const __watyPathIsAbsolute = (value) => {
            if (value === "") return false;
            if (__watyPathIsWin) {
                return /^[A-Za-z]:[\\/]/.test(value) || /^\\\\/.test(value);
            }
            return value.startsWith("/");
        };

        const __watyPathNormalizePosix = (value) => {
            if (value === "") return ".";
            if (/^\/+$/.test(value)) return value;
            const absolute = value.startsWith("/");
            const parts = value.split("/");
            const out = [];
            let trailing = false;
            let last = "";
            for (const part of parts) {
                if (part === "") { trailing = true; continue; }
                if (part === ".") { trailing = true; last = "."; continue; }
                if (part === "..") {
                    if (out.length && out[out.length - 1] !== "..") out.pop();
                    else if (!absolute) out.push("..");
                    trailing = true;
                    last = "..";
                    continue;
                }
                out.push(part);
                trailing = false;
                last = part;
            }
            let result = (absolute ? "/" : "") + out.join("/");
            if (result === "") return ".";
            if (result !== "/" && result !== "." && result !== ".." && trailing &&
                (last === "." || last === ".." || value.endsWith("/"))) {
                result += "/";
            }
            return result;
        };

        const __watyPathNormalizeWin = (value) => {
            if (value === "") return ".";
            let normalized = value.replace(/\//g, "\\");
            if (/^\\+$/.test(normalized)) return normalized;

            let prefix = "";
            let rest = normalized;
            const drive = /^([A-Za-z]:)/.exec(normalized);
            if (drive) {
                prefix = drive[1];
                rest = normalized.slice(2);
            } else if (normalized.startsWith("\\\\")) {
                prefix = "\\\\";
                rest = normalized.slice(2);
            }

            const absolute = rest.startsWith("\\");
            if (absolute) rest = rest.slice(1);
            const parts = rest.split("\\");
            const out = [];
            let trailing = false;
            let last = "";
            for (const part of parts) {
                if (part === "") { trailing = true; continue; }
                if (part === ".") { trailing = true; last = "."; continue; }
                if (part === "..") {
                    if (out.length && out[out.length - 1] !== "..") out.pop();
                    else if (!absolute) out.push("..");
                    trailing = true;
                    last = "..";
                    continue;
                }
                out.push(part);
                trailing = false;
                last = part;
            }

            let result = prefix + (absolute ? "\\" : "") + out.join("\\");
            if (result === "") return ".";
            if (result !== prefix + "\\" && result !== "." && result !== ".." && trailing &&
                (last === "." || last === ".." || value.endsWith("/") || value.endsWith("\\"))) {
                if (!result.endsWith("\\")) result += "\\";
            }
            return result;
        };

        const __watyPathNormalize = (value) =>
            __watyPathIsWin ? __watyPathNormalizeWin(value) : __watyPathNormalizePosix(value);

        const __watyPathJoin = (parts) => {
            let result = "";
            for (const raw of parts) {
                const part = __watyPathAssert(raw, "path.join 参数");
                if (result === "" || __watyPathIsAbsolute(part)) result = part;
                else if (result.endsWith(__watyPathSep)) result += part;
                else result += __watyPathSep + part;
            }
            return result === "" ? "." : __watyPathNormalize(result);
        };

        const __watyPathCwd = () => {
            if (!__watyPathData.cwdOk) {
                throw __watyPathError("PATH_NOT_AVAILABLE", __watyPathData.cwdError || "当前工作目录不可用");
            }
            return __watyPathData.cwd;
        };

        const __watyPathStripTrailing = (value) => {
            if (value.length > 1 && (value.endsWith("/") || value.endsWith("\\"))) {
                if (/^[A-Za-z]:[\\/]$/.test(value)) return value;
                return value.slice(0, -1);
            }
            return value;
        };

        const __watyPathResolve = (parts) => {
            let result = "";
            for (const raw of parts) {
                const part = __watyPathAssert(raw, "path.resolve 参数");
                if (part === "") continue;
                if (__watyPathIsAbsolute(part)) result = part;
                else if (result === "") result = part;
                else if (result.endsWith(__watyPathSep)) result += part;
                else result += __watyPathSep + part;
            }
            if (result === "") result = __watyPathCwd();
            else if (!__watyPathIsAbsolute(result)) result = __watyPathCwd() + __watyPathSep + result;
            return __watyPathStripTrailing(__watyPathNormalize(result));
        };

        const __watyPathBasename = (value, ext) => {
            const p = __watyPathAssert(value, "path.basename 参数");
            if (ext !== undefined && typeof ext !== "string") throw new TypeError("path.basename ext 必须是字符串");
            if (p === "" || __watyPathIsSep(p[p.length - 1])) return "";
            let start = p.length - 1;
            while (start >= 0 && !__watyPathIsSep(p[start])) start--;
            let name = p.slice(start + 1);
            if (ext && name !== ext && name.length > ext.length && name.endsWith(ext)) {
                name = name.slice(0, -ext.length);
            }
            return name;
        };

        const __watyPathDirname = (value) => {
            const p = __watyPathAssert(value, "path.dirname 参数");
            if (p === "") return ".";
            if ([...p].every(__watyPathIsSep)) return p;

            let end = p.length;
            while (end > 0 && __watyPathIsSep(p[end - 1])) end--;
            if (end < p.length) {
                let result = p.slice(0, end);
                if (/^[A-Za-z]:$/.test(result)) result += __watyPathSep;
                return result || ".";
            }

            let index = p.length - 1;
            while (index >= 0 && !__watyPathIsSep(p[index])) index--;
            if (index < 0) return ".";

            let result = p.slice(0, index);
            while (result.length > 1 && __watyPathIsSep(result[result.length - 1])) result = result.slice(0, -1);
            if (result === "") return __watyPathIsSep(p[0]) ? p[0] : ".";
            if (/^[A-Za-z]:$/.test(result)) result += __watyPathSep;
            return result;
        };

        const __watyPathExtname = (value) => {
            const p = __watyPathAssert(value, "path.extname 参数");
            const name = __watyPathBasename(p);
            if (name === "" || name === "." || name === "..") return "";
            const index = name.lastIndexOf(".");
            return index <= 0 ? "" : name.slice(index);
        };

        window.waty.path = {
            sep: __watyPathSep,
            delimiter: __watyPathDelimiter,
            cwd: () => __watyPathCwd(),
            normalize: (value) => __watyPathNormalize(__watyPathAssert(value, "path.normalize 参数")),
            join: (...parts) => __watyPathJoin(parts),
            resolve: (...parts) => __watyPathResolve(parts),
            basename: (value, ext) => __watyPathBasename(value, ext),
            dirname: (value) => __watyPathDirname(value),
            extname: (value) => __watyPathExtname(value),
            isAbsolute: (value) => __watyPathIsAbsolute(__watyPathAssert(value, "path.isAbsolute 参数")),
            getPath: (name) => {
                if (typeof name !== "string") throw new TypeError("path.getPath name 必须是字符串");
                const entry = __watyPathData.paths[name];
                if (!entry) throw __watyPathError("PATH_INVALID_ARGUMENT", "未知路径: " + name);
                if (!entry.ok) {
                    throw __watyPathError(entry.code || "PATH_NOT_AVAILABLE", entry.message || ("路径不可用: " + name));
                }
                return entry.value;
            }
        };

        window.waty.__watyPathSetCwd = (cwd, error) => {
            __watyPathData.cwd = typeof cwd === "string" ? cwd : "";
            __watyPathData.cwdError = typeof error === "string" ? error : "";
            __watyPathData.cwdOk = __watyPathData.cwd !== "" && __watyPathData.cwdError === "";
        };
    }

    // -- WINDOW 模块：所有操作都落到当前窗口的原生实现 ----------------------
    // 返回 Promise；窗口未 realize 时 C++ 侧返回 WINDOW_INTERNAL 错误。
    if (@@WINDOW@@) {
        const w = (op, args) => request("window", op, args ? { args } : {});
        window.waty.window = {
            setTitle: (t) => w("setTitle", [t]),
            getSize: () => w("getSize"),
            setPosition: (x, y) => w("setPosition", [x, y]),
            center: () => w("center"),
            maximize: () => w("maximize"),
            minimize: () => w("minimize"),
            restore: () => w("restore"),
            close: () => w("close"),
            hide: () => w("hide"),
            show: () => w("show"),
            focus: () => w("focus"),
            setAlwaysOnTop: (b) => w("setAlwaysOnTop", [b]),
            setDecorated: (b) => w("setDecorated", [b]),
            openDevTools: () => w("openDevTools"),
            isMaximized: () => w("isMaximized"),
            isMinimized: () => w("isMinimized"),
            isVisible: () => w("isVisible")
        };
    }

    // -- FS 模块：读/写权限可独立开启 -------------------------------------
    // readFile 返回 Uint8Array；createReadStream 基于 WHATWG ReadableStream，
    // 每块通过 fs/readStreamChunk 拉取（chunk 大小 = highWaterMark，默认 64KiB）。
    // 写流同理；流句柄保存在 C++ 全局表中，页面必须 close/cancel 释放。
    if (@@FS_READ@@ || @@FS_WRITE@@) {
        window.waty.fs = {};
        if (@@FS_READ@@) {
            window.waty.fs.readFile = async (path) => {
                const r = await request("fs", "readFile", { path });
                return bytesFromB64(r.base64);
            };
            window.waty.fs.stat = (path) => request("fs", "stat", { path });
            window.waty.fs.exists = (path) => request("fs", "exists", { path });
            window.waty.fs.createReadStream = async (path, options) => {
                const opened = await request("fs", "openReadStream", { path, highWaterMark: (options && options.highWaterMark) || 65536 });
                return new ReadableStream({
                    async pull(controller) {
                        const chunk = await request("fs", "readStreamChunk", { streamId: opened.streamId, size: opened.size });
                        if (chunk.eof) {
                            await request("fs", "closeStream", { streamId: opened.streamId });
                            controller.close();
                        } else {
                            controller.enqueue(bytesFromB64(chunk.base64));
                        }
                    },
                    cancel() { return request("fs", "closeStream", { streamId: opened.streamId }); }
                });
            };
        }
        if (@@FS_WRITE@@) {
            window.waty.fs.writeFile = async (path, data) => {
                await request("fs", "writeFile", { path, base64: b64FromBytes(data) });
            };
            window.waty.fs.createWriteStream = async (path) => {
                const opened = await request("fs", "openWriteStream", { path });
                return new WritableStream({
                    async write(chunk) {
                        await request("fs", "writeStreamChunk", { streamId: opened.streamId, base64: b64FromBytes(chunk) });
                    },
                    async close() { await request("fs", "closeStream", { streamId: opened.streamId }); },
                    async abort() { await request("fs", "closeStream", { streamId: opened.streamId }); }
                });
            };
        }
    }

    // -- DIALOG 模块：原生对话框，Promise 结果形状与 Electron 近似 ---------
    // showOpenDialog → { canceled, filePaths: string[] }
    // showSaveDialog → { canceled, filePath: string }
    // showMessageBox → { response: number, checkboxChecked: boolean }
    if (@@DIALOG@@) {
        window.waty.dialog = {
            showOpenDialog: (options) => request("dialog", "showOpenDialog", { options: options || {} }),
            showSaveDialog: (options) => request("dialog", "showSaveDialog", { options: options || {} }),
            showMessageBox: (options) => request("dialog", "showMessageBox", { options })
        };
    }

    // -- SQLITE3 模块：数据库按 dbName + version 全应用共享 ------------------
    // openDatabase 返回的 dbId 实际是 "<name>@<version>"；execute/query 使用命名参数，
    // params 对象的 key 可带或不带 : @ $ 前缀（C++ 侧逐个尝试）。
    // execute → { changes, lastInsertRowid }；query → { rows, columns }；
    // transaction → { ok: true }（单事务内按顺序执行，出错自动回滚）。
    if (@@SQLITE3@@) {
        window.waty.sqlite3 = {
            openDatabase: (dbName, version) => {
                if (typeof dbName !== "string" || !/^[A-Za-z0-9_]{1,64}$/.test(dbName)) {
                    return Promise.reject({
                        name: "WatySqlite3Error",
                        code: "SQLITE3_INVALID_ARGUMENT",
                        message: "dbName must match /^[A-Za-z0-9_]{1,64}$/"
                    });
                }
                if (typeof version !== "string" || !/^\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(version)) {
                    return Promise.reject({
                        name: "WatySqlite3Error",
                        code: "SQLITE3_INVALID_ARGUMENT",
                        message: "version must match /^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$/"
                    });
                }
                return request("sqlite3", "open", { dbName, version }).then((dbId) => ({
                    dbId,
                    execute: (sql, params) => request("sqlite3", "execute", { dbId, sql, params: params || {} }),
                    query: (sql, params) => request("sqlite3", "query", { dbId, sql, params: params || {} }),
                    transaction: (statements) => request("sqlite3", "transaction", { dbId, statements }),
                    close: () => request("sqlite3", "close", { dbId })
                }));
            }
        };
    }

    // -- NOTIFY 模块：系统通知 ---------------------------------------------
    // send(options, handlers) 中 handlers 可为函数（统一回调）或
    // { onAction, onClose, onError } 对象。C++ 侧把事件再发回 notify-event，
    // 由本段按 callbackId 路由；关闭/错误后自动清理回调，避免泄漏。
    if (@@NOTIFY@@) {
        let __watyNotifySeq = 0;
        const __watyNotifyCallbacks = Object.create(null);

        // C++ 侧通过 window.emit("notify-event", ...) 投递；直接挂内部 listener，
        // 不依赖 permissions.ipc，也不会与旧版 notify-action/closed/error 事件冲突。
        const __watyNotifyDispatch = (data) => {
            if (!data || !data.callbackId) return;
            const callback = __watyNotifyCallbacks[data.callbackId];
            if (!callback) return;
            if (data.event === "close" || data.event === "error") {
                delete __watyNotifyCallbacks[data.callbackId];
            }
            try {
                callback(data);
            } catch (error) {
                console.error("notify callback 异常", error);
            }
        };
        window.waty.__listeners["notify-event"] = window.waty.__listeners["notify-event"] || [];
        window.waty.__listeners["notify-event"].push(__watyNotifyDispatch);

        const __watyNotifyDispatcher = (callbackOrHandlers) => {
            if (callbackOrHandlers == null) {
                return null;
            }
            if (typeof callbackOrHandlers === "function") {
                return callbackOrHandlers;
            }
            if (typeof callbackOrHandlers !== "object") {
                throw new TypeError("notify.send 第二个参数必须是函数或 handlers 对象");
            }

            const onAction = callbackOrHandlers.onAction;
            const onClose  = callbackOrHandlers.onClose;
            const onError  = callbackOrHandlers.onError;
            for (const [name, handler] of [["onAction", onAction], ["onClose", onClose], ["onError", onError]]) {
                if (handler != null && typeof handler !== "function") {
                    throw new TypeError("notify.send " + name + " 必须是函数");
                }
            }
            if (onAction == null && onClose == null && onError == null) {
                throw new TypeError("notify.send handlers 至少需要 onAction / onClose / onError 之一");
            }

            return (event) => {
                if (event.event === "action" && onAction) onAction(event);
                else if (event.event === "close" && onClose) onClose(event);
                else if (event.event === "error" && onError) onError(event);
            };
        };

        window.waty.notify = {
            send: (options, callbackOrHandlers) => {
                let dispatcher;
                try {
                    dispatcher = __watyNotifyDispatcher(callbackOrHandlers);
                } catch (error) {
                    return Promise.reject(error);
                }

                let callbackId = 0;
                if (dispatcher) {
                    callbackId = ++__watyNotifySeq;
                    __watyNotifyCallbacks[callbackId] = dispatcher;
                }

                const promise = request("notify", "send", { options: options || {}, callbackId });
                if (callbackId !== 0) {
                    promise.catch(() => { delete __watyNotifyCallbacks[callbackId]; });
                }
                return promise;
            },
            hasPermission: () => request("notify", "hasPermission"),
            requestPermission: () => request("notify", "requestPermission")
        };
    }

    // -- 错误上报：页面未捕获异常 → C++ 触发 js-error-crash 事件 -----------
    // 这两个监听器不受权限开关影响（诊断信息始终可用）。
    window.addEventListener("error", (e) => {
        window.saucer.internal.message(JSON.stringify({ __waty: "jserror", message: e.message, source: e.filename, lineno: e.lineno, colno: e.colno }));
    });
    window.addEventListener("unhandledrejection", (e) => {
        let reason = e.reason;
        if (reason instanceof Error) reason = reason.message;
        window.saucer.internal.message(JSON.stringify({ __waty: "jserror", message: String(reason), source: "", lineno: 0, colno: 0 }));
    });

    // -- CONSOLE 转发：需 permissions.log ---------------------------------
    // 保留原始 console 行为，同时把参数（Error 取 message，其余 String()）发给 C++ 日志。
    if (@@LOG@@) {
        const sendLog = (level, args) => {
            try {
                window.saucer.internal.message(JSON.stringify({ __waty: "console", level, args: Array.from(args).map(a => (a instanceof Error ? a.message : String(a))) }));
            } catch (e) {}
        };
        const olog = console.log, owarn = console.warn, oerror = console.error;
        console.log = (...a) => { sendLog("log", a); olog.apply(console, a); };
        console.warn = (...a) => { sendLog("warn", a); owarn.apply(console, a); };
        console.error = (...a) => { sendLog("error", a); oerror.apply(console, a); };
    }
})();
)js";

        // 在脚本模板中替换所有占位符。占位符值均为合法的 JS 字面量
        // （true/false 或 JSON dump 结果），因此可以直接拼接。
        std::string replace_all(std::string s, std::string_view from, std::string_view to)
        {
            std::size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos)
            {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        }

        // 根据窗口权限生成最终注入脚本。
        // path 开启时会预先解析所有系统路径（失败也编码为 {ok:false, code, message}），
        // 并把 cwd 快照一并嵌入，保证页面首次调用 path 相关 API 时不需要跨桥往返。
        std::string build_js_script(const WebWindow::Impl &impl)
        {
            std::string script = k_bridge_script;
            const auto &p = impl.permissions;

            nlohmann::json paths = nlohmann::json::object();
            if (p.path)
            {
                const auto add_path = [&](const char *key, SysPath sys_path) {
                    try
                    {
                        paths[key] = {{"ok", true},
                                      {"value", path::getPath(sys_path, impl.organization_name, impl.app_name)}};
                    }
                    catch (const PathError &ex)
                    {
                        paths[key] = {{"ok", false}, {"code", ex.code()}, {"message", ex.what()}};
                    }
                    catch (const std::exception &ex)
                    {
                        paths[key] = {{"ok", false},
                                      {"code", "PATH_NOT_AVAILABLE"},
                                      {"message", ex.what()}};
                    }
                };

                add_path("appData", SysPath::AppData);
                add_path("config", SysPath::Config);
                add_path("temp", SysPath::Temp);
                add_path("downloads", SysPath::Downloads);
                add_path("documents", SysPath::Documents);
                add_path("executable", SysPath::Executable);
                add_path("home", SysPath::Home);
                add_path("cache", SysPath::Cache);
                add_path("logs", SysPath::Logs);
            }

            nlohmann::json path_data;
            path_data["paths"]    = std::move(paths);
            path_data["cwd"]      = impl.js_cwd;
            path_data["cwdOk"]    = !impl.js_cwd.empty() && impl.js_cwd_error.empty();
            path_data["cwdError"] = impl.js_cwd_error;

            script = replace_all(script, "@@IPC@@", p.ipc ? "true" : "false");
            script = replace_all(script, "@@OS@@", p.os ? "true" : "false");
            script = replace_all(script, "@@PATH@@", p.path ? "true" : "false");
            script = replace_all(script, "@@PATH_DATA@@", path_data.dump());
            script = replace_all(script, "@@PATH_SEP@@",
                                 nlohmann::json(std::string(1, path::sep())).dump());
            script = replace_all(script, "@@PATH_DELIMITER@@",
                                 nlohmann::json(std::string(path::delimiter())).dump());
            script = replace_all(script, "@@WINDOW@@", p.window ? "true" : "false");
            script = replace_all(script, "@@FS_READ@@", p.fs.read ? "true" : "false");
            script = replace_all(script, "@@FS_WRITE@@", p.fs.write ? "true" : "false");
            script = replace_all(script, "@@DIALOG@@", p.dialog ? "true" : "false");
            script = replace_all(script, "@@SQLITE3@@", p.sqlite3 ? "true" : "false");
            script = replace_all(script, "@@NOTIFY@@", p.notify ? "true" : "false");
            script = replace_all(script, "@@LOG@@", p.log ? "true" : "false");
            return script;
        }

        // C++ 异常 → JS 错误对象。
        // invalid_argument 映射为 IPC_INVALID_ARGUMENT，其它 std::exception 映射为
        // IPC_HANDLER_EXCEPTION；未知异常也归入后者。name 固定为 WatyIpcError。
        nlohmann::json ipc_error_from_exception(const std::exception_ptr &ptr)
        {
            try
            {
                std::rethrow_exception(ptr);
            }
            catch (const std::invalid_argument &ex)
            {
                return nlohmann::json{{"name", "WatyIpcError"}, {"code", "IPC_INVALID_ARGUMENT"}, {"message", ex.what()}};
            }
            catch (const std::exception &ex)
            {
                return nlohmann::json{{"name", "WatyIpcError"}, {"code", "IPC_HANDLER_EXCEPTION"}, {"message", ex.what()}};
            }
            catch (...)
            {
                return nlohmann::json{{"name", "WatyIpcError"}, {"code", "IPC_HANDLER_EXCEPTION"}, {"message", "Unknown exception"}};
            }
        }

        // 处理 waty.ipc.invoke / createInvoker 请求。
        // 流程：查 handler（单 handler 模型）→ 校验存活 → 在新线程执行用户回调
        //       → 等 future → post 回 UI 线程响应。
        // 找不到 handler / handler 已 unsubscribe 均返回 IPC_HANDLER_NOT_FOUND。
        void handle_ipc_request(WebWindow::Impl &impl, const nlohmann::json &req)
        {
            const std::uint64_t id = req.value("id", static_cast<std::uint64_t>(0));
            const std::string event = req.value("event", "");
            const nlohmann::json args = req.value("args", nlohmann::json::array());

            std::shared_ptr<WebWindow::Impl::IpcHandlerEntry> handler;
            {
                std::lock_guard<std::mutex> lock(impl.ipc_mutex);
                const auto it = impl.ipc_handlers.find(event);
                if (it == impl.ipc_handlers.end())
                {
                    respond_error(impl, id, nlohmann::json{{"name", "WatyIpcError"}, {"code", "IPC_HANDLER_NOT_FOUND"},
                                                           {"message", "No handler registered for '" + event + "'"}});
                    return;
                }
                handler = it->second;
            }

            bool alive = true;
            {
                std::lock_guard<std::mutex> lock(handler->mutex);
                alive = handler->alive;
            }
            if (!alive)
            {
                respond_error(impl, id, nlohmann::json{{"name", "WatyIpcError"}, {"code", "IPC_HANDLER_NOT_FOUND"},
                                                       {"message", "No handler registered for '" + event + "'"}});
                return;
            }

            const auto saucer_app = impl.saucer_app;
            WebWindow::Impl *impl_ptr = &impl;

            // 在线程池中执行用户回调（返回 std::future<json>）
            std::thread([handler, args, id, impl_ptr, saucer_app] {
                try
                {
                    auto future = handler->callback(args);
                    const auto result = future.get();
                    post_to_ui_thread(saucer_app, [impl_ptr, id, result] {
                        respond(*impl_ptr, id, result);
                    });
                }
                catch (...)
                {
                    const auto error = ipc_error_from_exception(std::current_exception());
                    post_to_ui_thread(saucer_app, [impl_ptr, id, error] {
                        respond_error(*impl_ptr, id, error);
                    });
                }
            }).detach();
        }

        // 处理 waty.os.platform() / waty.os.arch()。
        // 只做编译期常量映射，不产生任何系统调用；未知 op 返回 OS_INVALID_ARGUMENT。
        void handle_os_request(WebWindow::Impl &impl, const nlohmann::json &req)
        {
            const std::uint64_t id = req.value("id", static_cast<std::uint64_t>(0));
            const std::string op = req.value("op", "");

            if (op == "platform")
            {
#if defined(_WIN32)
                respond(impl, id, "win32");
#elif defined(__APPLE__)
                respond(impl, id, "darwin");
#else
                respond(impl, id, "linux");
#endif
                return;
            }
            if (op == "arch")
            {
#if defined(__x86_64__) || defined(_M_X64)
                respond(impl, id, "x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
                respond(impl, id, "arm64");
#else
                respond(impl, id, "unknown");
#endif
                return;
            }

            respond_error(impl, id, nlohmann::json{{"name", "WatyOsError"}, {"code", "OS_INVALID_ARGUMENT"}, {"message", "unknown op"}});
        }

        // 处理 waty.window.* 请求。
        // 参数从 args 数组按位置安全读取（类型不符时取默认值）；
        // 所有操作在 UI 线程执行（handle_js_message 本身在 UI 线程被调用）。
        // 失败统一返回 { name: WatyWindowError, code: WINDOW_INTERNAL|WINDOW_INVALID_ARGUMENT }。
        void handle_window_request(WebWindow::Impl &impl, const nlohmann::json &req)
        {
            const std::uint64_t id = req.value("id", static_cast<std::uint64_t>(0));
            const std::string op = req.value("op", "");
            const nlohmann::json args = req.value("args", nlohmann::json::array());

            const auto str_arg = [&](std::size_t i) -> std::string {
                if (args.is_array() && args.size() > i && args[i].is_string()) return args[i].get<std::string>();
                return {};
            };
            const auto int_arg = [&](std::size_t i) -> int {
                if (args.is_array() && args.size() > i && args[i].is_number()) return args[i].get<int>();
                return 0;
            };
            const auto bool_arg = [&](std::size_t i) -> bool {
                if (args.is_array() && args.size() > i && args[i].is_boolean()) return args[i].get<bool>();
                return false;
            };

            auto lock_window = [&impl]() -> saucer::window & {
                if (!impl.window) throw std::logic_error("window not realized");
                return *impl.window;
            };

            try
            {
                if (op == "setTitle") { lock_window().set_title(str_arg(0)); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "getSize") { const auto s = lock_window().size(); respond(impl, id, nlohmann::json{{"width", s.w}, {"height", s.h}}); return; }
                if (op == "setPosition") { lock_window().set_position({int_arg(0), int_arg(1)}); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "center")
                {
                    if (!impl.saucer_app) { respond(impl, id, nlohmann::json(nullptr)); return; }
                    const auto screens = impl.saucer_app->screens();
                    if (screens.empty()) { respond(impl, id, nlohmann::json(nullptr)); return; }
                    const auto &screen = screens.front();
                    const auto size = lock_window().size();
                    lock_window().set_position({screen.position.x + (screen.size.w - size.w) / 2,
                                                screen.position.y + (screen.size.h - size.h) / 2});
                    respond(impl, id, nlohmann::json(nullptr));
                    return;
                }
                if (op == "maximize") { lock_window().set_maximized(true); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "minimize") { lock_window().set_minimized(true); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "restore") { lock_window().set_maximized(false); lock_window().set_minimized(false); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "close") { lock_window().close(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "hide") { lock_window().hide(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "show") { lock_window().show(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "focus") { lock_window().focus(); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "setAlwaysOnTop") { lock_window().set_always_on_top(bool_arg(0)); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "setDecorated") { lock_window().set_decorations(bool_arg(0) ? saucer::window::decoration::full : saucer::window::decoration::partial); respond(impl, id, nlohmann::json(nullptr)); return; }
                if (op == "openDevTools")
                {
                    // 复用 C++ 侧能力（WebWindow::openDevTools，含 permissions.devtools 校验）
                    if (impl.app)
                    {
                        if (const auto window = impl.app->getWindow(impl.id))
                        {
                            (*window)->openDevTools();
                            respond(impl, id, nlohmann::json(nullptr));
                            return;
                        }
                    }
                    respond_error(impl, id, nlohmann::json{{"name", "WatyWindowError"}, {"code", "WINDOW_INTERNAL"}, {"message", "window not found"}});
                    return;
                }
                if (op == "isMaximized") { respond(impl, id, lock_window().maximized()); return; }
                if (op == "isMinimized") { respond(impl, id, lock_window().minimized()); return; }
                if (op == "isVisible") { respond(impl, id, lock_window().visible()); return; }

                respond_error(impl, id, nlohmann::json{{"name", "WatyWindowError"}, {"code", "WINDOW_INVALID_ARGUMENT"}, {"message", "unknown op"}});
            }
            catch (const std::exception &ex)
            {
                respond_error(impl, id, nlohmann::json{{"name", "WatyWindowError"}, {"code", "WINDOW_INTERNAL"}, {"message", ex.what()}});
            }
        }
    } // namespace

    // 向页面兑现 Promise。view 为空（窗口已销毁/未 realize）时丢弃。
    // 注意：result.dump() 必须产生合法 JS 字面量，因此调用方不能传 NaN/Infinity。
    void respond(WebWindow::Impl &impl, std::uint64_t id, const nlohmann::json &result)
    {
        if (!impl.view)
        {
            return;
        }
        impl.view->execute("window.__watyResolve(" + std::to_string(id) + "," + result.dump() + ");");
    }

    void respond_error(WebWindow::Impl &impl, std::uint64_t id, const nlohmann::json &error)
    {
        if (!impl.view)
        {
            return;
        }
        impl.view->execute("window.__watyReject(" + std::to_string(id) + "," + error.dump() + ");");
    }

    // 向页面推送事件（对应 JS 的 waty.ipc.on / 内部 notify-event 监听）。
    void emit_to_js(WebWindow::Impl &impl, std::string_view event, const nlohmann::json &data)
    {
        if (!impl.view)
        {
            return;
        }
        const std::string code = "window.__watyEmit(" + nlohmann::json(event).dump() + "," + data.dump() + ");";
        impl.view->execute(code);
    }

    // JS 消息总入口：按 "__waty" 字段分发到各通道。
    // 解析失败的 JSON 直接丢弃（不向前端报错，避免坏消息引发二次异常）。
    // jserror / console 是单向通道，无 id，不回响应。
    void handle_js_message(WebWindow::Impl &impl, std::string_view message)
    {
        nlohmann::json req;
        try
        {
            req = nlohmann::json::parse(message);
        }
        catch (...)
        {
            return;
        }

        const std::string channel = req.value("__waty", "");
        if (channel == "ipc")
        {
            handle_ipc_request(impl, req);
        }
        else if (channel == "os")
        {
            handle_os_request(impl, req);
        }
        else if (channel == "window")
        {
            handle_window_request(impl, req);
        }
        else if (channel == "fs")
        {
            handle_fs_request(impl, req);
        }
        else if (channel == "dialog")
        {
            handle_dialog_request(impl, req);
        }
        else if (channel == "sqlite3")
        {
            handle_sqlite3_request(impl, req);
        }
        else if (channel == "notify")
        {
            handle_notify_request(impl, req);
        }
        else if (channel == "jserror")
        {
            if (impl.app)
            {
                impl.app->emitInternalEvent("js-error-crash", req);
            }
        }
        else if (channel == "console")
        {
            // JS console 转发到 C++ 日志（文档 11，需 permissions.log）
            const std::string level = req.value("level", "log");
            std::string line;
            if (req.contains("args") && req["args"].is_array())
            {
                for (const auto &arg : req["args"])
                {
                    if (!line.empty())
                    {
                        line += " ";
                    }
                    line += arg.is_string() ? arg.get<std::string>() : arg.dump();
                }
            }
            log_message(level, "[js] " + line);
        }
    }

    // 注入 JS 桥并监听 WebView 的 message 事件。
    //   * 脚本在 "creation" 时机注入，保证页面脚本执行前 window.waty 已存在；
    //   * 只接管包含 "\"__waty\"" 的消息，其余消息返回 unhandled 交给页面/其它模块；
    //   * path 权限开启时额外注册 cwd 轮询任务，cwd 变化时推送给页面。
    void setup_js_bridge(WebWindow::Impl &impl)
    {
        if (impl.bridge_ready)
        {
            return;
        }
        impl.bridge_ready = true;

        if (impl.permissions.path)
        {
            try
            {
                impl.js_cwd = path::cwd();
                impl.js_cwd_error.clear();
            }
            catch (const std::exception &ex)
            {
                impl.js_cwd.clear();
                impl.js_cwd_error = ex.what();
            }
        }

        if (impl.view)
        {
            impl.view->inject({.code = build_js_script(impl), .run_at = saucer::script::time::creation, .clearable = false});
            impl.view->on<saucer::webview::event::message>({{.func = [impl_ptr = &impl](std::string_view message) -> saucer::status {
                if (message.find("\"__waty\"") != std::string_view::npos)
                {
                    handle_js_message(*impl_ptr, message);
                    return saucer::status::handled;
                }
                return saucer::status::unhandled;
            }, .clearable = false}});

            if (impl.permissions.path && impl.app)
            {
                impl.cwd_subscription = impl.app->onEventLoopTick([impl_ptr = &impl] {
                    if (!impl_ptr->view)
                    {
                        return;
                    }

                    std::string cwd;
                    std::string error;
                    try
                    {
                        cwd = path::cwd();
                    }
                    catch (const std::exception &ex)
                    {
                        error = ex.what();
                    }

                    if (cwd == impl_ptr->js_cwd && error == impl_ptr->js_cwd_error)
                    {
                        return;
                    }

                    impl_ptr->js_cwd       = cwd;
                    impl_ptr->js_cwd_error = error;

                    const auto code =
                        "if (window.waty && window.waty.__watyPathSetCwd) window.waty.__watyPathSetCwd(" +
                        nlohmann::json(cwd).dump() + "," + nlohmann::json(error).dump() + ");";
                    impl_ptr->view->execute(code);
                });
            }
        }
    }
} // namespace waty::detail

namespace waty
{
    // 注册 IPC handler（单 handler 模型）。
    // 同一事件名重复注册（且旧 handler 仍存活）会抛 std::logic_error；
    // 已 unsubscribe 的 handler 允许被覆盖，便于热重载/重新注册。
    Subscription WebWindow::ipcHandle(std::string_view event,
                                      std::function<std::future<nlohmann::json>(const nlohmann::json &)> callback)
    {
        auto entry = std::make_shared<WebWindow::Impl::IpcHandlerEntry>();
        entry->callback = std::move(callback);

        const std::string key(event);
        {
            std::lock_guard<std::mutex> lock(m_impl->ipc_mutex);
            const auto it = m_impl->ipc_handlers.find(key);
            if (it != m_impl->ipc_handlers.end())
            {
                bool alive = true;
                {
                    std::lock_guard<std::mutex> entry_lock(it->second->mutex);
                    alive = it->second->alive;
                }
                if (alive)
                {
                    throw std::logic_error("Attempted to register a second handler for '" + key + "'");
                }

                // 已 unsubscribe 的 dead handler：直接覆盖，允许该事件名重新注册。
                it->second = entry;
            }
            else
            {
                m_impl->ipc_handlers.emplace(key, entry);
            }
        }

        return Subscription([entry] {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->alive = false;
        });
    }

    // 向页面推送事件。已关闭窗口静默丢弃（记录 warning），避免对已销毁 view 调用 execute。
    void WebWindow::emit(std::string_view event, const nlohmann::json &data)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->view || m_impl->closed)
        {
            // 目标窗口已关闭：静默丢弃并记录警告（文档 7.1.4）
            detail::log_message("warn", "waty: emit 到已关闭窗口，消息丢弃");
            return;
        }
        detail::emit_to_js(*m_impl, event, data);
    }
} // namespace waty
