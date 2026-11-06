#pragma once

// 事件对象（见 DESIGN.md §7.2.2）。
//
// 事件回调签名统一为 void(Event&, const nlohmann::json&)：
//   * Event     —— 控制流开关，调用 preventDefault() 可阻止默认行为；
//   * json      —— 事件负载（字段见 DESIGN.md 对应事件表）。
//
// 每次事件分发会创建一个新的 Event 实例，多个监听器共享同一个实例：
// 任一监听器 preventDefault() 后，defaultPrevented() 对所有后续监听器可见。

namespace waty
{
    /// 事件对象（见文档 7.2.2），用于在回调中阻止默认行为。
    /// 当前生效的默认行为：应用退出（before-quit / will-quit）、关闭窗口（close）、
    /// 最后一个窗口关闭后自动退出（window-all-closed）。
    class Event
    {
      public:
        /// 阻止默认行为（如应用退出、关闭窗口、window-all-closed 的默认退出）
        void preventDefault()
        {
            m_prevented = true;
        }

        /// 是否已阻止默认行为
        bool defaultPrevented() const
        {
            return m_prevented;
        }

      private:
        bool m_prevented = false;
    };
} // namespace waty
