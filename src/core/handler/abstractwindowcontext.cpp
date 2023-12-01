#include "abstractwindowcontext_p.h"

namespace QWK {

    AbstractWindowContext::~AbstractWindowContext() {
        delete m_delegate;
    }

    void AbstractWindowContext::setupWindow(QWindow *window) {
        m_windowHandle = window;
    }

    bool AbstractWindowContext::setHitTestVisible(QObject *obj, bool visible) {
        Q_ASSERT(obj);
        if (!obj) {
            return false;
        }

        if (visible) {
            m_hitTestVisibleItems.insert(obj);
        } else {
            m_hitTestVisibleItems.remove(obj);
        }
        return true;
    }

    bool AbstractWindowContext::setHitTestVisible(const QRect &rect, bool visible) {
        Q_ASSERT(rect.isValid());
        if (!rect.isValid()) {
            return false;
        }

        if (visible) {
            m_hitTestVisibleRects.append(rect);
        } else {
            m_hitTestVisibleRects.removeAll(rect);
        }
        return true;
    }

    bool AbstractWindowContext::setSystemButton(CoreWindowAgent::SystemButton button,
                                                     QObject *obj) {
        Q_ASSERT(obj);
        Q_ASSERT(button != CoreWindowAgent::Unknown);
        if (!obj || (button == CoreWindowAgent::Unknown)) {
            return false;
        }

        if (m_systemButtons[button] == obj) {
            return false;
        }
        m_systemButtons[button] = obj;
        return true;
    }

    bool AbstractWindowContext::setTitleBar(QObject *item) {
        Q_ASSERT(item);
        if (!item) {
            return false;
        }

        if (m_titleBar == item) {
            return false;
        }
        m_titleBar = item;
        return true;
    }

    void AbstractWindowContext::showSystemMenu(const QPoint &pos) {
        // ?
    }

}