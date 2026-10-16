#pragma once

namespace waty
{
    /// 事件对象（见文档 7.2.2），用于在回调中阻止默认行为。
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
