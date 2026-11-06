#pragma once

// 原生对话框选项与结果（见 DESIGN.md §6.5）。
//
// 这些结构体同时用于 C++ API 与 JS 桥（字段名在 JS 侧转为 camelCase）：
//   * OpenDialogOptions  → waty.dialog.showOpenDialog(options)
//   * SaveDialogOptions  → waty.dialog.showSaveDialog(options)
//   * MessageBoxOptions  → waty.dialog.showMessageBox(options)
//
// 注意：字符串字段使用 std::string_view，调用方必须保证其生命周期覆盖对话框调用；
// JS 桥内部会先把字符串拷贝到局部 std::string 再赋值，避免临时对象悬空。

#include <string>
#include <string_view>
#include <vector>

namespace waty
{
    /// 文件类型过滤器（见文档 6.5）。
    /// 平台会将其转换为 "*.png;*.jpg" 之类的模式；extensions 不带前导点。
    struct FileFilter
    {
        std::string name;                     // 显示名称，如 "Images"
        std::vector<std::string> extensions;  // 扩展名，如 {"png", "jpg", "jpeg"}
    };

    /// 打开文件对话框选项（见文档 6.5）
    struct OpenDialogOptions
    {
        std::string_view title;        // 对话框标题
        std::string_view defaultPath;  // 默认路径，仅支持文件系统路径
        bool multiSelect = false;      // 是否允许多选
        bool showHidden  = false;      // 是否显示隐藏文件
        bool directory   = false;      // 是否选择目录而非文件

        std::vector<FileFilter> filters;
    };

    /// 保存文件对话框选项（见文档 6.5）
    struct SaveDialogOptions
    {
        std::string_view title;
        std::string_view defaultPath;
        std::string_view defaultName;

        std::vector<FileFilter> filters;
    };

    /// 消息框类型（见文档 6.5）。
    /// 用于选择平台图标（信息/警告/错误/问号）；None 表示不显示图标。
    enum class MessageBoxType
    {
        None,
        Info,
        Warning,
        Error,
        Question,
    };

    /// 原生消息框选项（见文档 6.5）
    struct MessageBoxOptions
    {
        std::string_view title;
        std::string_view message;
        std::string_view detail;                       // 可选，详细信息
        MessageBoxType type = MessageBoxType::None;
        std::vector<std::string> buttons;              // 按钮文本列表，至少一个
        int defaultButtonIndex = 0;                    // 默认按钮索引（回车触发）
        int cancelButtonIndex  = -1;                   // 取消按钮索引（Esc 触发），-1 表示无取消按钮
        std::string_view checkboxLabel;                // 可选，复选框文本
        bool checkboxChecked = false;                  // 可选，复选框初始状态
        bool modal           = true;                   // 是否模态
    };

    /// 原生消息框结果（见文档 6.5）。
    /// Linux 的 GtkAlertDialog 不支持复选框，checkboxChecked 恒为 false。
    struct MessageBoxResult
    {
        int response;              // 用户点击的按钮索引（0 起始）；标题栏关闭时为 -1
        bool checkboxChecked;      // 复选框最终状态；未启用复选框时为 false
    };
} // namespace waty
