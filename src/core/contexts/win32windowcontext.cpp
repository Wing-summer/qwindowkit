#include "win32windowcontext_p.h"

#include <optional>

#include <QtCore/QHash>
#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QCoreApplication>
#include <QtCore/QOperatingSystemVersion>

#include <QtCore/private/qsystemlibrary_p.h>
#include <QtGui/private/qhighdpiscaling_p.h>

#include "qwkcoreglobal_p.h"

#include <shellscalingapi.h>
#include <dwmapi.h>

namespace QWK {

    static constexpr const auto kAutoHideTaskBarThickness = quint8{ 2 }; // The thickness of an auto-hide taskbar in pixels.

    using WndProcHash = QHash<HWND, Win32WindowContext *>; // hWnd -> context
    Q_GLOBAL_STATIC(WndProcHash, g_wndProcHash);

    static WNDPROC g_qtWindowProc = nullptr; // Original Qt window proc function

    struct DynamicApis {
        decltype(&::DwmFlush) pDwmFlush = nullptr;
        decltype(&::GetDpiForWindow) pGetDpiForWindow = nullptr;
        decltype(&::GetSystemMetricsForDpi) pGetSystemMetricsForDpi = nullptr;
        decltype(&::GetDpiForMonitor) pGetDpiForMonitor = nullptr;

        DynamicApis() {
            QSystemLibrary user32(QStringLiteral("user32.dll"));
            pGetDpiForWindow = reinterpret_cast<decltype(pGetDpiForWindow)>(user32.resolve("GetDpiForWindow"));
            pGetSystemMetricsForDpi = reinterpret_cast<decltype(pGetSystemMetricsForDpi)>(user32.resolve("GetSystemMetricsForDpi"));

            QSystemLibrary shcore(QStringLiteral("shcore.dll"));
            pGetDpiForMonitor = reinterpret_cast<decltype(pGetDpiForMonitor)>(shcore.resolve("GetDpiForMonitor"));

            QSystemLibrary dwmapi(QStringLiteral("dwmapi.dll"));
            pDwmFlush = reinterpret_cast<decltype(pDwmFlush)>(dwmapi.resolve("DwmFlush"));
        }

        ~DynamicApis() = default;

        static const DynamicApis &instance() {
            static const DynamicApis inst{};
            return inst;
        }

    private:
        Q_DISABLE_COPY_MOVE(DynamicApis)
    };

    static inline constexpr bool operator==(const POINT &lhs, const POINT &rhs) noexcept
    {
        return ((lhs.x == rhs.x) && (lhs.y == rhs.y));
    }

    static inline constexpr bool operator!=(const POINT &lhs, const POINT &rhs) noexcept
    {
        return !operator==(lhs, rhs);
    }

    static inline constexpr bool operator==(const SIZE &lhs, const SIZE &rhs) noexcept
    {
        return ((lhs.cx == rhs.cx) && (lhs.cy == rhs.cy));
    }

    static inline constexpr bool operator!=(const SIZE &lhs, const SIZE &rhs) noexcept
    {
        return !operator==(lhs, rhs);
    }

    static inline constexpr bool operator>(const SIZE &lhs, const SIZE &rhs) noexcept
    {
        return ((lhs.cx * lhs.cy) > (rhs.cx * rhs.cy));
    }

    static inline constexpr bool operator>=(const SIZE &lhs, const SIZE &rhs) noexcept
    {
        return (operator>(lhs, rhs) || operator==(lhs, rhs));
    }

    static inline constexpr bool operator<(const SIZE &lhs, const SIZE &rhs) noexcept
    {
        return (operator!=(lhs, rhs) && !operator>(lhs, rhs));
    }

    static inline constexpr bool operator<=(const SIZE &lhs, const SIZE &rhs) noexcept
    {
        return (operator<(lhs, rhs) || operator==(lhs, rhs));
    }

    static inline constexpr bool operator==(const RECT &lhs, const RECT &rhs) noexcept
    {
        return ((lhs.left == rhs.left) && (lhs.top == rhs.top) && (lhs.right == rhs.right) && (lhs.bottom == rhs.bottom));
    }

    static inline constexpr bool operator!=(const RECT &lhs, const RECT &rhs) noexcept
    {
        return !operator==(lhs, rhs);
    }

    static inline constexpr QPoint point2qpoint(const POINT &point)
    {
        return QPoint{ int(point.x), int(point.y) };
    }

    static inline constexpr POINT qpoint2point(const QPoint &point)
    {
        return POINT{ LONG(point.x()), LONG(point.y()) };
    }

    static inline constexpr QSize size2qsize(const SIZE &size)
    {
        return QSize{ int(size.cx), int(size.cy) };
    }

    static inline constexpr SIZE qsize2size(const QSize &size)
    {
        return SIZE{ LONG(size.width()), LONG(size.height()) };
    }

    static inline constexpr QRect rect2qrect(const RECT &rect)
    {
        return QRect{ QPoint{ int(rect.left), int(rect.top) }, QSize{ int(RECT_WIDTH(rect)), int(RECT_HEIGHT(rect)) } };
    }

    static inline constexpr RECT qrect2rect(const QRect &qrect)
    {
        return RECT{ LONG(qrect.left()), LONG(qrect.top()), LONG(qrect.right()), LONG(qrect.bottom()) };
    }

    static inline /*constexpr*/ QString hwnd2str(const WId windowId)
    {
        // NULL handle is allowed here.
        return QLatin1String("0x") + QString::number(windowId, 16).toUpper().rightJustified(8, u'0');
    }

    static inline /*constexpr*/ QString hwnd2str(const HWND hwnd)
    {
        // NULL handle is allowed here.
        return hwnd2str(reinterpret_cast<WId>(hwnd));
    }

    static inline bool isWin8Point1OrGreater() {
        static const bool result = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows8_1;
        return result;
    }

    static inline bool isWin10OrGreater() {
        static const bool result = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows10;
        return result;
    }

    static inline quint32 getDpiForWindow(const HWND hwnd) {
        Q_ASSERT(hwnd);
        if (!hwnd) {
            return USER_DEFAULT_SCREEN_DPI;
        }
        const DynamicApis &apis = DynamicApis::instance();
        if (apis.pGetDpiForWindow) { // Win10
            return apis.pGetDpiForWindow(hwnd);
        } else if (apis.pGetDpiForMonitor) { // Win8.1
            const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            UINT dpiX{ USER_DEFAULT_SCREEN_DPI };
            UINT dpiY{ USER_DEFAULT_SCREEN_DPI };
            apis.pGetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
            return dpiX;
        } else { // Win2K
            const HDC hdc = ::GetDC(nullptr);
            const int dpiX = ::GetDeviceCaps(hdc, LOGPIXELSX);
            const int dpiY = ::GetDeviceCaps(hdc, LOGPIXELSY);
            ::ReleaseDC(nullptr, hdc);
            return quint32(dpiX);
        }
    }

    static inline quint32 getResizeBorderThickness(const HWND hwnd) {
        Q_ASSERT(hwnd);
        if (!hwnd) {
            return 0;
        }
        const DynamicApis &apis = DynamicApis::instance();
        if (apis.pGetSystemMetricsForDpi) {
            const quint32 dpi = getDpiForWindow(hwnd);
            return apis.pGetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + apis.pGetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        } else {
            return ::GetSystemMetrics(SM_CXSIZEFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
        }
    }

    static inline std::optional<MONITORINFOEXW> getMonitorForWindow(const HWND hwnd)
    {
        Q_ASSERT(hwnd);
        if (!hwnd) {
            return std::nullopt;
        }
        // Use "MONITOR_DEFAULTTONEAREST" here so that we can still get the correct
        // monitor even if the window is minimized.
        const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        ::GetMonitorInfoW(monitor, &monitorInfo);
        return monitorInfo;
    };

    static inline bool isFullScreen(const HWND hwnd) {
        Q_ASSERT(hwnd);
        if (!hwnd) {
            return false;
        }
        RECT windowRect{};
        ::GetWindowRect(hwnd, &windowRect);
        const std::optional<MONITORINFOEXW> mi = getMonitorForWindow(hwnd);
        // Compare to the full area of the screen, not the work area.
        return (windowRect == mi.value_or(MONITORINFOEXW{}).rcMonitor);
    }

    static inline QPoint fromNativeLocalPosition(const QWindow *window, const QPoint &point) {
        Q_ASSERT(window);
        if (!window) {
            return point;
        }
#if 1
        return QHighDpi::fromNativeLocalPosition(point, window);
#else
        return QPointF(QPointF(point) / window->devicePixelRatio()).toPoint();
#endif
    }

    static inline Win32WindowContext::WindowPart getHitWindowPart(int hitTestResult) {
        switch (hitTestResult) {
            case HTCLIENT:
                return Win32WindowContext::ClientArea;
            case HTCAPTION:
                return Win32WindowContext::TitleBar;
            case HTSYSMENU:
            case HTHELP:
            case HTREDUCE:
            case HTZOOM:
            case HTCLOSE:
                return Win32WindowContext::ChromeButton;
            case HTLEFT:
            case HTRIGHT:
            case HTTOP:
            case HTTOPLEFT:
            case HTTOPRIGHT:
            case HTBOTTOM:
            case HTBOTTOMLEFT:
            case HTBOTTOMRIGHT:
                return Win32WindowContext::ResizeBorder;
            case HTBORDER:
                return Win32WindowContext::FixedBorder;
            default:
                break;
        }
        return Win32WindowContext::Outside;
    }

    static bool isValidWindow(WId windowId, bool checkVisible, bool checkTopLevel) {
        const auto hwnd = reinterpret_cast<HWND>(windowId);
        if (::IsWindow(hwnd) == FALSE) {
            return false;
        }
        const LONG_PTR styles = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (styles & WS_DISABLED) {
            return false;
        }
        const LONG_PTR exStyles = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyles & WS_EX_TOOLWINDOW) {
            return false;
        }
        RECT rect = {0, 0, 0, 0};
        if (::GetWindowRect(hwnd, &rect) == FALSE) {
            return false;
        }
        if ((rect.left >= rect.right) || (rect.top >= rect.bottom)) {
            return false;
        }
        if (checkVisible) {
            if (::IsWindowVisible(hwnd) == FALSE) {
                return false;
            }
        }
        if (checkTopLevel) {
            if (::GetAncestor(hwnd, GA_ROOT) != hwnd) {
                return false;
            }
        }
        return true;
    }

    // https://github.com/qt/qtbase/blob/e26a87f1ecc40bc8c6aa5b889fce67410a57a702/src/plugins/platforms/windows/qwindowscontext.cpp#L1556
    // In QWindowsContext::windowsProc(), the messages will be passed to all global native event
    // filters, but because we have already filtered the messages in the hook WndProc function for
    // convenience, Qt does not know we may have already process the messages and thus will call
    // DefWindowProc(). Consequently, we have to add a global native filter that forwards the result
    // of the hook function, telling Qt whether we have filtered the events before. Since Qt only
    // handles Windows window messages in the main thread, it is safe to do so.
    class WindowsNativeEventFilter : public QAbstractNativeEventFilter {
    public:
        bool nativeEventFilter(const QByteArray &eventType, void *message,
                               QT_NATIVE_EVENT_RESULT_TYPE *result) override {
            // It has been observed that the pointer that Qt gives us is sometimes null on some machines.
            // We need to guard against it in such scenarios.
            if (!result) {
                return false;
            }
            if (lastMessageHandled) {
                *result = static_cast<QT_NATIVE_EVENT_RESULT_TYPE>(lastMessageResult);
                return true;
            }
            return false;
        }

        static bool lastMessageHandled;
        static LRESULT lastMessageResult;
        static WindowsNativeEventFilter *instance;

        static inline void install() {
            instance = new WindowsNativeEventFilter();
            qApp->installNativeEventFilter(instance);
        }

        static inline void uninstall() {
            qApp->removeNativeEventFilter(instance);
            delete instance;
            instance = nullptr;
        }
    };

    bool WindowsNativeEventFilter::lastMessageHandled = false;
    LRESULT WindowsNativeEventFilter::lastMessageResult = 0;
    WindowsNativeEventFilter *WindowsNativeEventFilter::instance = nullptr;

    // https://github.com/qt/qtbase/blob/e26a87f1ecc40bc8c6aa5b889fce67410a57a702/src/plugins/platforms/windows/qwindowscontext.cpp#L1025
    // We can see from the source code that Qt will filter out some messages first and then send the
    // unfiltered messages to the event dispatcher. To activate the Snap Layout feature on Windows
    // 11, we must process some non-client area messages ourselves, but unfortunately these messages
    // have been filtered out already in that line, and thus we'll never have the chance to process
    // them ourselves. This is Qt's low level platform specific code, so we don't have any official
    // ways to change this behavior. But luckily we can replace the window procedure function of
    // Qt's windows, and in this hooked window procedure function, we finally have the chance to
    // process window messages before Qt touches them. So we reconstruct the MSG structure and send
    // it to our own custom native event filter to do all the magic works. But since the system menu
    // feature doesn't necessarily belong to the native implementation, we seperate the handling
    // code and always process the system menu part in this function for both implementations.
    //
    // Original event flow:
    //      [Entry]             Windows Message Queue
    //                          |
    //      [Qt Window Proc]    qwindowscontext.cpp#L1547: qWindowsWndProc()
    //                              ```
    //                              const bool handled = QWindowsContext::instance()->windowsProc
    //                                  (hwnd, message, et, wParam, lParam, &result,
    //                                  &platformWindow);
    //                              ```
    //                          |
    //      [Non-Input Filter]  qwindowscontext.cpp#L1025: QWindowsContext::windowsProc()
    //                              ```
    //                              if (!isInputMessage(msg.message) &&
    //                                  filterNativeEvent(&msg, result))
    //                                  return true;
    //                              ```
    //                          |
    //      [User Filter]       qwindowscontext.cpp#L1588: QWindowsContext::windowsProc()
    //                              ```
    //                              QAbstractEventDispatcher *dispatcher =
    //                              QAbstractEventDispatcher::instance();
    //                              qintptr filterResult = 0;
    //                              if (dispatcher &&
    //                              dispatcher->filterNativeEvent(nativeEventType(), msg,
    //                              &filterResult)) {
    //                                  *result = LRESULT(filterResult);
    //                                  return true;
    //                              }
    //                              ```
    //                          |
    //      [Extra work]        The rest of QWindowsContext::windowsProc() and qWindowsWndProc()
    //
    // Notice: Only non-input messages will be processed by the user-defined global native event
    // filter!!! These events are then passed to the widget class's own overridden
    // QWidget::nativeEvent() as a local filter, where all native events can be handled, but we must
    // create a new class derived from QWidget which we don't intend to. Therefore, we don't expect
    // to process events from the global native event filter, but instead hook Qt's window
    // procedure.

    extern "C" LRESULT QT_WIN_CALLBACK QWKHookedWndProc(HWND hWnd, UINT message, WPARAM wParam,
                                                        LPARAM lParam) {
        Q_ASSERT(hWnd);
        if (!hWnd) {
            return FALSE;
        }

        // Search window context
        auto ctx = g_wndProcHash->value(hWnd);
        if (!ctx) {
            return ::DefWindowProcW(hWnd, message, wParam, lParam);
        }

        // Try hooked procedure and save result
        auto &handled = WindowsNativeEventFilter::lastMessageHandled;
        auto &result = WindowsNativeEventFilter::lastMessageResult;
        handled = ctx->windowProc(hWnd, message, wParam, lParam, &result);

        // TODO: Determine whether to show system menu
        // ...

        // Since Qt does the necessary processing of the message afterward, we still need to
        // continue dispatching it.
        return ::CallWindowProcW(g_qtWindowProc, hWnd, message, wParam, lParam);
    }

    Win32WindowContext::Win32WindowContext(QWindow *window, WindowItemDelegate *delegate)
        : AbstractWindowContext(window, delegate) {
    }

    Win32WindowContext::~Win32WindowContext() {
        // Remove window handle mapping
        if (auto hWnd = reinterpret_cast<HWND>(windowId); hWnd) {
            g_wndProcHash->remove(hWnd);

            // Remove event filter if the all windows has been destroyed
            if (g_wndProcHash->empty()) {
                WindowsNativeEventFilter::uninstall();
            }
        }
    }

    bool Win32WindowContext::setup() {
        auto winId = m_windowHandle->winId();

        // Install window hook
        auto hWnd = reinterpret_cast<HWND>(winId);

        // Store original window proc
        if (!g_qtWindowProc) {
            g_qtWindowProc = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(hWnd, GWLP_WNDPROC));
        }

        // Hook window proc
        ::SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(QWKHookedWndProc));

        // Install global native event filter
        if (!WindowsNativeEventFilter::instance) {
            WindowsNativeEventFilter::install();
        }

        // Cache window ID
        windowId = winId;

        // Save window handle mapping
        g_wndProcHash->insert(hWnd, this);

        return true;
    }

    bool Win32WindowContext::windowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                        LRESULT *result) {
        *result = FALSE;

        // We should skip these messages otherwise we will get crashes.
        // NOTE: WM_QUIT won't be posted to the WindowProc function.
        switch (message) {
            case WM_CLOSE:
            case WM_DESTROY:
            case WM_NCDESTROY:
            // Undocumented messages:
            case WM_UAHDESTROYWINDOW:
            case WM_UNREGISTER_WINDOW_SERVICES:
                return false;
            default:
                break;
        }

        if (!isValidWindow(windowId, false, true)) {
            return false;
        }

        // Test snap layout
        if (snapLayoutHandler(hWnd, message, wParam, lParam, result)) {
            return true;
        }

        // Main implementation
        if (customWindowHandler(hWnd, message, wParam, lParam, result)) {
            return true;
        }

        return false; // Not handled
    }

    static constexpr const auto kMessageTag = WPARAM(0x97CCEA99);

    static inline constexpr bool isTaggedMessage(WPARAM wParam) {
        return (wParam == kMessageTag);
    }

    static inline quint64 getKeyState() {
        quint64 result = 0;
        const auto &get = [](const int virtualKey) -> bool {
            return (::GetAsyncKeyState(virtualKey) < 0);
        };
        const bool buttonSwapped = (::GetSystemMetrics(SM_SWAPBUTTON) != FALSE);
        if (get(VK_LBUTTON)) {
            result |= (buttonSwapped ? MK_RBUTTON : MK_LBUTTON);
        }
        if (get(VK_RBUTTON)) {
            result |= (buttonSwapped ? MK_LBUTTON : MK_RBUTTON);
        }
        if (get(VK_SHIFT)) {
            result |= MK_SHIFT;
        }
        if (get(VK_CONTROL)) {
            result |= MK_CONTROL;
        }
        if (get(VK_MBUTTON)) {
            result |= MK_MBUTTON;
        }
        if (get(VK_XBUTTON1)) {
            result |= MK_XBUTTON1;
        }
        if (get(VK_XBUTTON2)) {
            result |= MK_XBUTTON2;
        }
        return result;
    }

    static void emulateClientAreaMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                         const std::optional<int> &overrideMessage = std::nullopt) {
        const int myMsg = overrideMessage.value_or(message);
        const auto wParamNew = [myMsg, wParam]() -> WPARAM {
            if (myMsg == WM_NCMOUSELEAVE) {
                // wParam is always ignored in mouse leave messages, but here we
                // give them a special tag to be able to distinguish which messages
                // are sent by ourselves.
                return kMessageTag;
            }
            const quint64 keyState = getKeyState();
            if ((myMsg >= WM_NCXBUTTONDOWN) && (myMsg <= WM_NCXBUTTONDBLCLK)) {
                const auto xButtonMask = GET_XBUTTON_WPARAM(wParam);
                return MAKEWPARAM(keyState, xButtonMask);
            }
            return keyState;
        }();
        const auto lParamNew = [myMsg, lParam, hWnd]() -> LPARAM {
            if (myMsg == WM_NCMOUSELEAVE) {
                // lParam is always ignored in mouse leave messages.
                return 0;
            }
            const auto screenPos = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT clientPos = screenPos;
            ::ScreenToClient(hWnd, &clientPos);
            return MAKELPARAM(clientPos.x, clientPos.y);
        }();
#if 0
#  define SEND_MESSAGE ::SendMessageW
#else
#  define SEND_MESSAGE ::PostMessageW
#endif
        switch (myMsg) {
            case WM_NCHITTEST: // Treat hit test messages as mouse move events.
            case WM_NCMOUSEMOVE:
                SEND_MESSAGE(hWnd, WM_MOUSEMOVE, wParamNew, lParamNew);
                break;
            case WM_NCLBUTTONDOWN:
                SEND_MESSAGE(hWnd, WM_LBUTTONDOWN, wParamNew, lParamNew);
                break;
            case WM_NCLBUTTONUP:
                SEND_MESSAGE(hWnd, WM_LBUTTONUP, wParamNew, lParamNew);
                break;
            case WM_NCLBUTTONDBLCLK:
                SEND_MESSAGE(hWnd, WM_LBUTTONDBLCLK, wParamNew, lParamNew);
                break;
            case WM_NCRBUTTONDOWN:
                SEND_MESSAGE(hWnd, WM_RBUTTONDOWN, wParamNew, lParamNew);
                break;
            case WM_NCRBUTTONUP:
                SEND_MESSAGE(hWnd, WM_RBUTTONUP, wParamNew, lParamNew);
                break;
            case WM_NCRBUTTONDBLCLK:
                SEND_MESSAGE(hWnd, WM_RBUTTONDBLCLK, wParamNew, lParamNew);
                break;
            case WM_NCMBUTTONDOWN:
                SEND_MESSAGE(hWnd, WM_MBUTTONDOWN, wParamNew, lParamNew);
                break;
            case WM_NCMBUTTONUP:
                SEND_MESSAGE(hWnd, WM_MBUTTONUP, wParamNew, lParamNew);
                break;
            case WM_NCMBUTTONDBLCLK:
                SEND_MESSAGE(hWnd, WM_MBUTTONDBLCLK, wParamNew, lParamNew);
                break;
            case WM_NCXBUTTONDOWN:
                SEND_MESSAGE(hWnd, WM_XBUTTONDOWN, wParamNew, lParamNew);
                break;
            case WM_NCXBUTTONUP:
                SEND_MESSAGE(hWnd, WM_XBUTTONUP, wParamNew, lParamNew);
                break;
            case WM_NCXBUTTONDBLCLK:
                SEND_MESSAGE(hWnd, WM_XBUTTONDBLCLK, wParamNew, lParamNew);
                break;
#if 0 // ### TODO: How to handle touch events?
        case WM_NCPOINTERUPDATE:
        case WM_NCPOINTERDOWN:
        case WM_NCPOINTERUP:
            break;
#endif
            case WM_NCMOUSEHOVER:
                SEND_MESSAGE(hWnd, WM_MOUSEHOVER, wParamNew, lParamNew);
                break;
            case WM_NCMOUSELEAVE:
                SEND_MESSAGE(hWnd, WM_MOUSELEAVE, wParamNew, lParamNew);
                break;
            default:
                break;
        }

#undef SEND_MESSAGE
    }

    static inline void requestForMouseLeaveMessage(HWND hWnd, bool nonClient) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        if (nonClient) {
            tme.dwFlags |= TME_NONCLIENT;
        }
        tme.hwndTrack = hWnd;
        tme.dwHoverTime = HOVER_DEFAULT;
        ::TrackMouseEvent(&tme);
    }

    bool Win32WindowContext::snapLayoutHandler(HWND hWnd, UINT message, WPARAM wParam,
                                               LPARAM lParam, LRESULT *result) {
        switch (message) {
            case WM_MOUSELEAVE: {
                if (!isTaggedMessage(wParam)) {
                    // Qt will call TrackMouseEvent() to get the WM_MOUSELEAVE message when it
                    // receives WM_MOUSEMOVE messages, and since we are converting every
                    // WM_NCMOUSEMOVE message to WM_MOUSEMOVE message and send it back to the window
                    // to be able to hover our controls, we also get lots of WM_MOUSELEAVE messages
                    // at the same time because of the reason above, and these superfluous mouse
                    // leave events cause Qt to think the mouse has left the control, and thus we
                    // actually lost the hover state. So we filter out these superfluous mouse leave
                    // events here to avoid this issue.
                    DWORD dwScreenPos = ::GetMessagePos();
                    QPoint qtScenePos =
                        fromNativeLocalPosition(m_windowHandle, QPoint(GET_X_LPARAM(dwScreenPos),
                                                                       GET_Y_LPARAM(dwScreenPos)));
                    auto dummy = CoreWindowAgent::Unknown;
                    if (isInSystemButtons(qtScenePos, &dummy)) {
                        // We must record whether the last WM_MOUSELEAVE was filtered, because if
                        // Qt does not receive this message it will not call TrackMouseEvent()
                        // again, resulting in the client area not responding to any mouse event.
                        mouseLeaveBlocked = true;
                        *result = FALSE;
                        return true;
                    }
                }
                mouseLeaveBlocked = false;
                break;
            }

            case WM_MOUSEMOVE: {
                // At appropriate time, we will call TrackMouseEvent() for Qt. Simultaneously,
                // we unset `mouseLeaveBlocked` mark and pretend as if Qt has received
                // WM_MOUSELEAVE.
                if (lastHitTestResult != WindowPart::ChromeButton && mouseLeaveBlocked) {
                    mouseLeaveBlocked = false;
                    requestForMouseLeaveMessage(hWnd, false);
                }
                break;
            }

            case WM_NCMOUSEMOVE:
            case WM_NCLBUTTONDOWN:
            case WM_NCLBUTTONUP:
            case WM_NCLBUTTONDBLCLK:
            case WM_NCRBUTTONDOWN:
            case WM_NCRBUTTONUP:
            case WM_NCRBUTTONDBLCLK:
            case WM_NCMBUTTONDOWN:
            case WM_NCMBUTTONUP:
            case WM_NCMBUTTONDBLCLK:
            case WM_NCXBUTTONDOWN:
            case WM_NCXBUTTONUP:
            case WM_NCXBUTTONDBLCLK:
#if 0 // ### TODO: How to handle touch events?
    case WM_NCPOINTERUPDATE:
    case WM_NCPOINTERDOWN:
    case WM_NCPOINTERUP:
#endif
            case WM_NCMOUSEHOVER: {
                const WindowPart currentWindowPart = lastHitTestResult;
                if (message == WM_NCMOUSEMOVE) {
                    if (currentWindowPart != WindowPart::ChromeButton) {
                        std::ignore = m_delegate->resetQtGrabbedControl();
                        if (mouseLeaveBlocked) {
                            emulateClientAreaMessage(hWnd, message, wParam, lParam,
                                                     WM_NCMOUSELEAVE);
                        }
                    }

                    // We need to make sure we get the right hit-test result when a WM_NCMOUSELEAVE
                    // comes, so we reset it when we receive a WM_NCMOUSEMOVE.

                    // If the mouse is entering the client area, there must be a WM_NCHITTEST
                    // setting it to `Client` before the WM_NCMOUSELEAVE comes; if the mouse is
                    // leaving the window, current window part remains as `Outside`.
                    lastHitTestResult = WindowPart::Outside;
                }

                if (currentWindowPart == WindowPart::ChromeButton) {
                    emulateClientAreaMessage(hWnd, message, wParam, lParam);
                    if (message == WM_NCMOUSEMOVE) {
                        // ### FIXME FIXME FIXME
                        // ### FIXME: Calling DefWindowProc() here is really dangerous, investigate
                        // how to avoid doing this.
                        // ### FIXME FIXME FIXME
                        *result = ::DefWindowProcW(hWnd, WM_NCMOUSEMOVE, wParam, lParam);
                    } else {
                        // According to MSDN, we should return non-zero for X button messages to
                        // indicate we have handled these messages (due to historical reasons), for
                        // all other messages we should return zero instead.
                        *result =
                            (((message >= WM_NCXBUTTONDOWN) && (message <= WM_NCXBUTTONDBLCLK))
                                 ? TRUE
                                 : FALSE);
                    }
                    return true;
                }
                break;
            }

            case WM_NCMOUSELEAVE: {
                const WindowPart currentWindowPart = lastHitTestResult;
                if (currentWindowPart == WindowPart::ChromeButton) {
                    // If we press on the chrome button and move mouse, Windows will take the
                    // pressing area as HTCLIENT which maybe because of our former retransmission of
                    // WM_NCLBUTTONDOWN, as a result, a WM_NCMOUSELEAVE will come immediately and a
                    // lot of WM_MOUSEMOVE will come if we move the mouse, we should track the mouse
                    // in advance.
                    if (mouseLeaveBlocked) {
                        mouseLeaveBlocked = false;
                        requestForMouseLeaveMessage(hWnd, false);
                    }
                } else {
                    if (mouseLeaveBlocked) {
                        // The mouse is moving from the chrome button to other non-client area, we
                        // should emulate a WM_MOUSELEAVE message to reset the button state.
                        emulateClientAreaMessage(hWnd, message, wParam, lParam, WM_NCMOUSELEAVE);
                    }

                    if (currentWindowPart == WindowPart::Outside) {
                        // Notice: we're not going to clear window part cache when the mouse leaves
                        // window from client area, which means we will get previous window part as
                        // HTCLIENT if the mouse leaves window from client area and enters window
                        // from non-client area, but it has no bad effect.
                        std::ignore = m_delegate->resetQtGrabbedControl();
                    }
                }
                break;
            }

            default:
                break;
        }
        return false;
    }

    bool Win32WindowContext::customWindowHandler(HWND hWnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam, LRESULT *result)
    {
        switch (message) {
            case WM_NCCALCSIZE: {
                // Windows是根据这个消息的返回值来设置窗口的客户区（窗口中真正显示的内容）
                // 和非客户区（标题栏、窗口边框、菜单栏和状态栏等Windows系统自行提供的部分
                // ，不过对于Qt来说，除了标题栏和窗口边框，非客户区基本也都是自绘的）的范
                // 围的，lParam里存放的就是新客户区的几何区域，默认是整个窗口的大小，正常
                // 的程序需要修改这个参数，告知系统窗口的客户区和非客户区的范围（一般来说可
                // 以完全交给Windows，让其自行处理，使用默认的客户区和非客户区），因此如果
                // 我们不修改lParam，就可以使客户区充满整个窗口，从而去掉标题栏和窗口边框
                // （因为这些东西都被客户区给盖住了。但边框阴影也会因此而丢失，不过我们会使
                // 用其他方式将其带回，请参考其他消息的处理，此处不过多提及）。但有个情况要
                // 特别注意，那就是窗口最大化后，窗口的实际尺寸会比屏幕的尺寸大一点，从而使
                // 用户看不到窗口的边界，这样用户就不能在窗口最大化后调整窗口的大小了（虽然
                // 这个做法听起来特别奇怪，但Windows确实就是这样做的），因此如果我们要自行
                // 处理窗口的非客户区，就要在窗口最大化后，将窗口边框的宽度和高度（一般是相
                // 等的）从客户区裁剪掉，否则我们窗口所显示的内容就会超出屏幕边界，显示不全。
                // 如果用户开启了任务栏自动隐藏，在窗口最大化后，还要考虑任务栏的位置。因为
                // 如果窗口最大化后，其尺寸和屏幕尺寸相等（因为任务栏隐藏了，所以窗口最大化
                // 后其实是充满了整个屏幕，变相的全屏了），Windows会认为窗口已经进入全屏的
                // 状态，从而导致自动隐藏的任务栏无法弹出。要避免这个状况，就要使窗口的尺寸
                // 小于屏幕尺寸。我下面的做法参考了火狐、Chromium和Windows Terminal
                // 如果没有开启任务栏自动隐藏，是不存在这个问题的，所以要先进行判断。
                // 一般情况下，*result设置为0（相当于DefWindowProc的返回值为0）就可以了，
                // 根据MSDN的说法，返回0意为此消息已经被程序自行处理了，让Windows跳过此消
                // 息，否则Windows会添加对此消息的默认处理，对于当前这个消息而言，就意味着
                // 标题栏和窗口边框又会回来，这当然不是我们想要的结果。根据MSDN，当wParam
                // 为FALSE时，只能返回0，但当其为TRUE时，可以返回0，也可以返回一个WVR_常
                // 量。根据Chromium的注释，当存在非客户区时，如果返回WVR_REDRAW会导致子
                // 窗口/子控件出现奇怪的bug（自绘控件错位），并且Lucas在Windows 10
                // 上成功复现，说明这个bug至今都没有解决。我查阅了大量资料，发现唯一的解决
                // 方案就是返回0。但如果不存在非客户区，且wParam为TRUE，最好返回
                // WVR_REDRAW，否则窗口在调整大小可能会产生严重的闪烁现象。
                // 虽然对大多数消息来说，返回0都代表让Windows忽略此消息，但实际上不同消息
                // 能接受的返回值是不一样的，请注意自行查阅MSDN。

                // Sent when the size and position of a window's client area must be
                // calculated. By processing this message, an application can
                // control the content of the window's client area when the size or
                // position of the window changes. If wParam is TRUE, lParam points
                // to an NCCALCSIZE_PARAMS structure that contains information an
                // application can use to calculate the new size and position of the
                // client rectangle. If wParam is FALSE, lParam points to a RECT
                // structure. On entry, the structure contains the proposed window
                // rectangle for the window. On exit, the structure should contain
                // the screen coordinates of the corresponding window client area.
                // The client area is the window's content area, the non-client area
                // is the area which is provided by the system, such as the title
                // bar, the four window borders, the frame shadow, the menu bar, the
                // status bar, the scroll bar, etc. But for Qt, it draws most of the
                // window area (client + non-client) itself. We now know that the
                // title bar and the window frame is in the non-client area and we
                // can set the scope of the client area in this message, so we can
                // remove the title bar and the window frame by let the non-client
                // area be covered by the client area (because we can't really get
                // rid of the non-client area, it will always be there, all we can
                // do is to hide it) , which means we should let the client area's
                // size the same with the whole window's size. So there is no room
                // for the non-client area and then the user won't be able to see it
                // again. But how to achieve this? Very easy, just leave lParam (the
                // re-calculated client area) untouched. But of course you can
                // modify lParam, then the non-client area will be seen and the
                // window borders and the window frame will show up. However, things
                // are quite different when you try to modify the top margin of the
                // client area. DWM will always draw the whole title bar no matter
                // what margin value you set for the top, unless you don't modify it
                // and remove the whole top area (the title bar + the one pixel
                // height window border). This can be confirmed in Windows
                // Terminal's source code, you can also try yourself to verify
                // it. So things will become quite complicated if you want to
                // preserve the four window borders.

                // If `wParam` is `FALSE`, `lParam` points to a `RECT` that contains
                // the proposed window rectangle for our window. During our
                // processing of the `WM_NCCALCSIZE` message, we are expected to
                // modify the `RECT` that `lParam` points to, so that its value upon
                // our return is the new client area. We must return 0 if `wParam`
                // is `FALSE`.
                // If `wParam` is `TRUE`, `lParam` points to a `NCCALCSIZE_PARAMS`
                // struct. This struct contains an array of 3 `RECT`s, the first of
                // which has the exact same meaning as the `RECT` that is pointed to
                // by `lParam` when `wParam` is `FALSE`. The remaining `RECT`s, in
                // conjunction with our return value, can
                // be used to specify portions of the source and destination window
                // rectangles that are valid and should be preserved. We opt not to
                // implement an elaborate client-area preservation technique, and
                // simply return 0, which means "preserve the entire old client area
                // and align it with the upper-left corner of our new client area".
                const auto clientRect = ((wParam == FALSE) ? reinterpret_cast<LPRECT>(lParam) : &(reinterpret_cast<LPNCCALCSIZE_PARAMS>(lParam))->rgrc[0]);
                if (isWin10OrGreater()) {
                    // Store the original top margin before the default window procedure applies the default frame.
                    const LONG originalTop = clientRect->top;
                    // Apply the default frame because we don't want to remove the whole window frame,
                    // we still need the standard window frame (the resizable frame border and the frame
                    // shadow) for the left, bottom and right edges.
                    // If we return 0 here directly, the whole window frame will be removed (which means
                    // there will be no resizable frame border and the frame shadow will also disappear),
                    // and that's also how most applications customize their title bars on Windows. It's
                    // totally OK but since we want to preserve as much original frame as possible, we
                    // can't use that solution.
                    const LRESULT hitTestResult = ::DefWindowProcW(hWnd, WM_NCCALCSIZE, wParam, lParam);
                    if ((hitTestResult != HTERROR) && (hitTestResult != HTNOWHERE)) {
                        *result = hitTestResult;
                        return true;
                    }
                    // Re-apply the original top from before the size of the default frame was applied,
                    // and the whole top frame (the title bar and the top border) is gone now.
                    // For the top frame, we only has 2 choices: (1) remove the top frame entirely, or
                    // (2) don't touch it at all. We can't preserve the top border by adjusting the top
                    // margin here. If we try to modify the top margin, the original title bar will
                    // always be painted by DWM regardless what margin we set, so here we can only remove
                    // the top frame entirely and use some special technique to bring the top border back.
                    clientRect->top = originalTop;
                }
                const bool max = IsMaximized(hWnd);
                const bool full = isFullScreen(hWnd);
                // We don't need this correction when we're fullscreen. We will
                // have the WS_POPUP size, so we don't have to worry about
                // borders, and the default frame will be fine.
                if (max && !full) {
                    // When a window is maximized, its size is actually a little bit more
                    // than the monitor's work area. The window is positioned and sized in
                    // such a way that the resize handles are outside the monitor and
                    // then the window is clipped to the monitor so that the resize handle
                    // do not appear because you don't need them (because you can't resize
                    // a window when it's maximized unless you restore it).
                    const quint32 frameSize = getResizeBorderThickness(hWnd);
                    clientRect->top += frameSize;
                    if (!isWin10OrGreater()) {
                        clientRect->bottom -= frameSize;
                        clientRect->left += frameSize;
                        clientRect->right -= frameSize;
                    }
                }
                // Attempt to detect if there's an autohide taskbar, and if
                // there is, reduce our size a bit on the side with the taskbar,
                // so the user can still mouse-over the taskbar to reveal it.
                // Make sure to use MONITOR_DEFAULTTONEAREST, so that this will
                // still find the right monitor even when we're restoring from
                // minimized.
                if (max || full) {
                    APPBARDATA abd;
                    SecureZeroMemory(&abd, sizeof(abd));
                    abd.cbSize = sizeof(abd);
                    const UINT taskbarState = ::SHAppBarMessage(ABM_GETSTATE, &abd);
                    // First, check if we have an auto-hide taskbar at all:
                    if (taskbarState & ABS_AUTOHIDE) {
                        bool top = false, bottom = false, left = false, right = false;
                        // Due to ABM_GETAUTOHIDEBAREX was introduced in Windows 8.1,
                        // we have to use another way to judge this if we are running
                        // on Windows 7 or Windows 8.
                        if (isWin8Point1OrGreater()) {
                            const std::optional<MONITORINFOEXW> monitorInfo = getMonitorForWindow(hWnd);
                            const RECT monitorRect = monitorInfo.value().rcMonitor;
                            // This helper can be used to determine if there's an
                            // auto-hide taskbar on the given edge of the monitor
                            // we're currently on.
                            const auto hasAutohideTaskbar = [monitorRect](const UINT edge) -> bool {
                                APPBARDATA abd2{};
                                abd2.cbSize = sizeof(abd2);
                                abd2.uEdge = edge;
                                abd2.rc = monitorRect;
                                const auto hTaskbar = reinterpret_cast<HWND>(::SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &abd2));
                                return (hTaskbar != nullptr);
                            };
                            top = hasAutohideTaskbar(ABE_TOP);
                            bottom = hasAutohideTaskbar(ABE_BOTTOM);
                            left = hasAutohideTaskbar(ABE_LEFT);
                            right = hasAutohideTaskbar(ABE_RIGHT);
                        } else {
                            int edge = -1;
                            APPBARDATA abd2{};
                            abd2.cbSize = sizeof(abd2);
                            abd2.hWnd = ::FindWindowW(L"Shell_TrayWnd", nullptr);
                            const HMONITOR windowMonitor = ::MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                            const HMONITOR taskbarMonitor = ::MonitorFromWindow(abd2.hWnd, MONITOR_DEFAULTTOPRIMARY);
                            if (taskbarMonitor == windowMonitor) {
                                ::SHAppBarMessage(ABM_GETTASKBARPOS, &abd2);
                                edge = abd2.uEdge;
                            }
                            top = (edge == ABE_TOP);
                            bottom = (edge == ABE_BOTTOM);
                            left = (edge == ABE_LEFT);
                            right = (edge == ABE_RIGHT);
                        }
                        // If there's a taskbar on any side of the monitor, reduce
                        // our size a little bit on that edge.
                        // Note to future code archeologists:
                        // This doesn't seem to work for fullscreen on the primary
                        // display. However, testing a bunch of other apps with
                        // fullscreen modes and an auto-hiding taskbar has
                        // shown that _none_ of them reveal the taskbar from
                        // fullscreen mode. This includes Edge, Firefox, Chrome,
                        // Sublime Text, PowerPoint - none seemed to support this.
                        // This does however work fine for maximized.
                        if (top) {
                            // Peculiarly, when we're fullscreen,
                            clientRect->top += kAutoHideTaskBarThickness;
                        } else if (bottom) {
                            clientRect->bottom -= kAutoHideTaskBarThickness;
                        } else if (left) {
                            clientRect->left += kAutoHideTaskBarThickness;
                        } else if (right) {
                            clientRect->right -= kAutoHideTaskBarThickness;
                        }
                    }
                }
                // ### TODO: std::ignore = Utils::syncWmPaintWithDwm(); // This should be executed at the very last.
                // By returning WVR_REDRAW we can make the window resizing look less broken.
                // But we must return 0 if wParam is FALSE, according to Microsoft Docs.
                // **IMPORTANT NOTE**:
                // If you are drawing something manually through D3D in your window, don't
                // try to return WVR_REDRAW here, otherwise Windows exhibits bugs where
                // client pixels and child windows are mispositioned by the width/height
                // of the upper-left non-client area. It's confirmed that this issue exists
                // from Windows 7 to Windows 10. Not tested on Windows 11 yet. Don't know
                // whether it exists on Windows XP to Windows Vista or not.
                *result = wParam == FALSE ? FALSE : WVR_REDRAW;
                return true;
            }
            default:
                break;
        }
        return false;
    }

}