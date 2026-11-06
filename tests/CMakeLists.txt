# ---------------------------------------------------------------------------
# waty 单元测试
#
# 本文件由根 CMakeLists.txt 在 WATY_BUILD_TESTS=ON 时通过
# add_subdirectory(tests) 引入；测试目标与 ctest 注册都在这里定义。
#
# 目录约定：
#   * 本目录存放测试源码；
#   * 需要直接编译的库源码位于 ${PROJECT_SOURCE_DIR}/src；
#   * 公共头文件位于 ${PROJECT_SOURCE_DIR}/include。
#
# 当前测试项目：
#   sqlite3 —— Sqlite3Service / 语句封装（连接共享、窗口级所有权、事务、并发）
#   base64  —— base64 编解码（fs 桥 / SQLite BLOB 的二进制传输格式）
#   paths   —— waty::path 词法工具、SysPath/getPath、目录名校验
#   core    —— Event / Subscription 语义与跨线程取消
#   tray    —— Menu / MenuItem / Icon 纯数据层（不创建平台托盘）
#   notify  —— notification 字段校验与平台辅助接口
#
# 说明：除 sqlite3 外均不依赖 WebView / Saucer；notify 仅链接 waty_notify
# 对象库（校验失败路径不会初始化后端）。
# ---------------------------------------------------------------------------

set(WATY_SRC_DIR "${PROJECT_SOURCE_DIR}/src")
set(WATY_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")

# --- Sqlite3：服务层 + 语句封装 + base64（BLOB 返回格式） ---
add_executable(waty_sqlite3_tests
    sqlite3_tests.cpp
    ${WATY_SRC_DIR}/base64.cpp
    ${WATY_SRC_DIR}/sqlite3_database.cpp
    ${WATY_SRC_DIR}/sqlite3_service.cpp
)
target_compile_features(waty_sqlite3_tests PRIVATE cxx_std_23)
target_include_directories(waty_sqlite3_tests PRIVATE ${WATY_SRC_DIR})
target_link_libraries(waty_sqlite3_tests PRIVATE SQLite::SQLite3 nlohmann_json::nlohmann_json)
add_test(NAME sqlite3 COMMAND waty_sqlite3_tests)

# --- base64（内部工具） ---
add_executable(waty_base64_tests
    base64_tests.cpp
    ${WATY_SRC_DIR}/base64.cpp
)
target_compile_features(waty_base64_tests PRIVATE cxx_std_23)
target_include_directories(waty_base64_tests PRIVATE ${WATY_SRC_DIR})
add_test(NAME base64 COMMAND waty_base64_tests)

# --- paths（公共 API + 内部校验） ---
add_executable(waty_paths_tests
    paths_tests.cpp
    ${WATY_SRC_DIR}/paths.cpp
)
target_compile_features(waty_paths_tests PRIVATE cxx_std_23)
target_include_directories(waty_paths_tests PRIVATE ${WATY_INCLUDE_DIR} ${WATY_SRC_DIR})
add_test(NAME paths COMMAND waty_paths_tests)

# --- core：Event / Subscription（header-only） ---
add_executable(waty_core_tests core_tests.cpp)
target_compile_features(waty_core_tests PRIVATE cxx_std_23)
target_include_directories(waty_core_tests PRIVATE ${WATY_INCLUDE_DIR})
add_test(NAME core COMMAND waty_core_tests)

# --- tray：Menu / Icon（header-only + menu.cpp，不链接平台后端） ---
add_executable(waty_tray_tests
    tray_tests.cpp
    ${WATY_SRC_DIR}/tray/common/menu.cpp
)
target_compile_features(waty_tray_tests PRIVATE cxx_std_23)
target_include_directories(waty_tray_tests PRIVATE ${WATY_INCLUDE_DIR})
add_test(NAME tray COMMAND waty_tray_tests)

# --- notify：send() 校验 + 平台辅助接口 ---
add_executable(waty_notify_tests notify_tests.cpp)
target_compile_features(waty_notify_tests PRIVATE cxx_std_23)
target_include_directories(waty_notify_tests PRIVATE ${WATY_INCLUDE_DIR})
target_link_libraries(waty_notify_tests PRIVATE waty_notify)
add_test(NAME notify COMMAND waty_notify_tests)
