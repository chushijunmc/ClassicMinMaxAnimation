// ClassicMinMax - injected hook DLL
// Animation logic adapted from the Windhawk mod "Classic Minimize/Maximize Animations"
// by aubymori (MIT licensed).
#define NOMINMAX
#include <windows.h>
#include <shellscalingapi.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdarg>

static void DbgLog(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    HANDLE h = CreateFileW(L"C:\\Users\\Public\\ClassicMinMax.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        char line[1200];
        int n = _snprintf_s(line, _TRUNCATE, "[%lu %llu] %s\r\n", GetCurrentProcessId(), GetTickCount64(), buf);
        DWORD written = 0;
        WriteFile(h, line, (DWORD)n, &written, nullptr);
        CloseHandle(h);
    }
}

#define RECTWIDTH(rc)  ((rc).right - (rc).left)
#define RECTHEIGHT(rc) ((rc).bottom - (rc).top)
#define MIN2(x, y)     (((x) > (y)) ? (y) : (x))
#define MAX2(x, y)     (((x) > (y)) ? (x) : (y))

static const wchar_t* kRegKey = L"Software\\ClassicMinMax";

static DWORD RegGetDword(const wchar_t* name, DWORD def)
{
    DWORD v = def, cb = sizeof(v), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, &type, &v, &cb) != ERROR_SUCCESS)
        return def;
    return v;
}

/* ShowWindow / ShowWindowAsync / SystemParametersInfo 会被高频调用，
   每次调用都 RegGetValue 会拖慢宿主进程（资源管理器还原窗口时尤其明显）。
   这里做 1 秒缓存，托盘开关依然能在 1 秒内生效。 */
static volatile LONG  g_enabledCached   = 1;
static volatile DWORD g_settingsTick    = 0;

static bool IsEnabled()
{
    DWORD now = GetTickCount();
    if ((DWORD)(now - g_settingsTick) >= 1000)
    {
        g_settingsTick  = now;
        g_enabledCached = RegGetDword(L"Enabled", 1) != 0 ? 1 : 0;
    }
    return g_enabledCached != 0;
}

static bool TopmostFixOn()  { return RegGetDword(L"TopmostFix", 0) != 0; }

static bool g_fDebugTrace = false;
#define TRACE(...) do { if (g_fDebugTrace) DbgLog(__VA_ARGS__); } while (0)

/* Globals */
static UINT       g_uMsgMinMax     = (UINT)-1;
static bool       g_fAnimating     = false;
static bool       g_fDisabled      = false;
static HINSTANCE  g_hinst          = nullptr;
static HWND       g_hwndAnim       = nullptr;
static HANDLE     g_hAnimWndThread = nullptr;

typedef BOOL  (WINAPI *GetWindowMinimizeRect_t)(HWND hwnd, LPRECT prcMin);
typedef HICON (WINAPI *InternalGetWindowIcon_t)(HWND hwnd, UINT uIconType);
typedef BOOL  (WINAPI *AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);

/* Win10 才有的 DPI API：Win7/8.1 上必须动态解析，否则 DLL 静态导入失败、注入直接无效 */
typedef DPI_AWARENESS_CONTEXT (WINAPI *GetWindowDpiAwarenessContext_t)(HWND);
typedef BOOL  (WINAPI *AreDpiAwarenessContextsEqual_t)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
typedef UINT  (WINAPI *GetDpiForWindow_t)(HWND);
typedef int   (WINAPI *GetSystemMetricsForDpi_t)(int, UINT);
typedef BOOL  (WINAPI *SystemParametersInfoForDpi_t)(UINT, UINT, PVOID, BOOL, UINT);
typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

static GetWindowMinimizeRect_t      pGetWindowMinimizeRect;
static InternalGetWindowIcon_t      pInternalGetWindowIcon;
static AdjustWindowRectExForDpi_t   pAdjustWindowRectExForDpi;
static GetWindowDpiAwarenessContext_t pGetWindowDpiAwarenessContext;
static AreDpiAwarenessContextsEqual_t pAreDpiAwarenessContextsEqual;
static GetDpiForWindow_t            pGetDpiForWindow;
static GetSystemMetricsForDpi_t      pGetSystemMetricsForDpi;
static SystemParametersInfoForDpi_t pSystemParametersInfoForDpi;
static SetThreadDpiAwarenessContext_t pSetThreadDpiAwarenessContext;
static GetDpiForMonitor_t           pGetDpiForMonitor;

typedef struct _MINMAXPARAMS
{
    HWND  hwnd;
    HICON hIcon;
    WCHAR szCaptionText[256];
    RECT  rcMax;
} MINMAXPARAMS, *LPMINMAXPARAMS;

static UINT DpiForWindowOf(HWND hwnd)
{
    if (pGetDpiForWindow)
        return pGetDpiForWindow(hwnd);

    /* Win7/8.1：没有 GetDpiForWindow，用屏幕 DPI 近似 */
    HDC hdc = GetDC(nullptr);
    UINT dpi = 96;
    if (hdc)
    {
        int px = GetDeviceCaps(hdc, LOGPIXELSX);
        if (px > 0)
            dpi = (UINT)px;
        ReleaseDC(nullptr, hdc);
    }
    return dpi;
}

static int MetricsForDpiOf(int nIndex, UINT dpi)
{
    if (pGetSystemMetricsForDpi)
        return pGetSystemMetricsForDpi(nIndex, dpi);
    return MulDiv(GetSystemMetrics(nIndex), (int)dpi, 96);
}

static BOOL SpiForDpi(UINT uiAction, UINT uiParam, PVOID pvParam, UINT dpi)
{
    if (pSystemParametersInfoForDpi)
        return pSystemParametersInfoForDpi(uiAction, uiParam, pvParam, FALSE, dpi);
    return SystemParametersInfoW(uiAction, uiParam, pvParam, 0);
}

static HRESULT GetDpiForMonitorOf(HMONITOR hmon, UINT* px, UINT* py)
{
    if (pGetDpiForMonitor)
        return pGetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, px, py);
    *px = 96;
    *py = 96;
    return E_NOTIMPL;
}

static bool IsWindowPerMonitorDpiAware(HWND hwnd)
{
    if (!pGetWindowDpiAwarenessContext || !pAreDpiAwarenessContextsEqual)
        return false;

    DPI_AWARENESS_CONTEXT ctx = pGetWindowDpiAwarenessContext(hwnd);
    return (pAreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) ||
            pAreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
}

static int BordersForDpi(DWORD style, DWORD exStyle, BOOL fWindow, BOOL fClient, UINT dpi)
{
    (void)fClient;
    if (!fWindow)
        return 0;
    RECT rc = { 0, 0, 100, 100 };
    if (pAdjustWindowRectExForDpi && pAdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, dpi))
    {
        int w = RECTWIDTH(rc) - 100;
        return (w > 0) ? (w / 2) : 0;
    }
    return GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

// Fallback when user32!GetWindowMinimizeRect is unavailable.
static void FallbackMinimizeRect(HWND hwnd, LPRECT prcMin)
{
    RECT rcWnd = { 0, 0, 0, 0 };
    GetWindowRect(hwnd, &rcWnd);

    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT rcTray = { 0, 0, 0, 0 };
    if (hTray)
        GetWindowRect(hTray, &rcTray);

    int cx = GetSystemMetrics(SM_CXSMICON) * 2;
    int cy = cx;
    int wx = (rcWnd.left + rcWnd.right) / 2;
    int wy = (rcWnd.top + rcWnd.bottom) / 2;

    int x, y;
    if (hTray && RECTHEIGHT(rcTray) < RECTWIDTH(rcTray))
    {
        // horizontal taskbar
        x = wx;
        if (x < rcTray.left + cx) x = rcTray.left + cx;
        if (x > rcTray.right - cx) x = rcTray.right - cx;
        y = (rcTray.top + rcTray.bottom) / 2;
    }
    else if (hTray)
    {
        y = wy;
        if (y < rcTray.top + cy) y = rcTray.top + cy;
        if (y > rcTray.bottom - cy) y = rcTray.bottom - cy;
        x = (rcTray.left + rcTray.right) / 2;
    }
    else
    {
        x = wx;
        y = GetSystemMetrics(SM_CYSCREEN);
    }
    SetRect(prcMin, x - cx / 2, y - cy / 2, x + cx / 2, y + cy / 2);
}

static void GetWindowMinimizeRectForWindowDpi(HWND hwnd, LPRECT prcMin)
{
    if (pGetWindowMinimizeRect)
        pGetWindowMinimizeRect(hwnd, prcMin);
    else
        FallbackMinimizeRect(hwnd, prcMin);

    if (!IsWindowPerMonitorDpiAware(hwnd))
    {
        MONITORINFO mi = { sizeof(mi) };
        HMONITOR hmon = MonitorFromRect(prcMin, MONITOR_DEFAULTTOPRIMARY);
        if (hmon && GetMonitorInfoW(hmon, &mi))
        {
            UINT dpiX = 96, dpiY = 96;
            if (GetDpiForMonitorOf(hmon, &dpiX, &dpiY) == S_OK)
            {
                UINT uDpiWnd = DpiForWindowOf(hwnd);
                prcMin->left   = MulDiv(prcMin->left,   uDpiWnd, dpiX);
                prcMin->top    = MulDiv(prcMin->top,    uDpiWnd, dpiY);
                prcMin->right  = MulDiv(prcMin->right,  uDpiWnd, dpiX);
                prcMin->bottom = MulDiv(prcMin->bottom, uDpiWnd, dpiY);
            }
        }
    }
}

static LPMINMAXPARAMS CreateMinMaxParams(HWND hwnd, UINT cmd)
{
    LPMINMAXPARAMS lpmmp = (LPMINMAXPARAMS)GlobalAlloc(GPTR, sizeof(MINMAXPARAMS));
    if (!lpmmp)
        return nullptr;

    lpmmp->hwnd  = hwnd;
    lpmmp->hIcon = pInternalGetWindowIcon ? pInternalGetWindowIcon(hwnd, ICON_SMALL) : nullptr;

    lpmmp->szCaptionText[0] = L'\0';
    GetWindowTextW(hwnd, lpmmp->szCaptionText, ARRAYSIZE(lpmmp->szCaptionText));

    if (cmd == SC_MAXIMIZE || cmd == SC_RESTORE)
    {
        MINMAXINFO mmi = {};
        int cBorders = BordersForDpi(
            GetWindowLongW(hwnd, GWL_STYLE),
            GetWindowLongW(hwnd, GWL_EXSTYLE),
            TRUE, FALSE, DpiForWindowOf(hwnd));

        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hmon, &mi);
        InflateRect(&mi.rcWork, cBorders, cBorders);

        mmi.ptMaxPosition.x = mi.rcWork.left;
        mmi.ptMaxPosition.y = mi.rcWork.top;
        mmi.ptMaxSize.x     = RECTWIDTH(mi.rcWork);
        mmi.ptMaxSize.y     = RECTHEIGHT(mi.rcWork);

        SendMessageW(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
        lpmmp->rcMax.left   = mmi.ptMaxPosition.x;
        lpmmp->rcMax.top    = mmi.ptMaxPosition.y;
        lpmmp->rcMax.right  = mmi.ptMaxPosition.x + mmi.ptMaxSize.x;
        lpmmp->rcMax.bottom = mmi.ptMaxPosition.y + mmi.ptMaxSize.y;
    }
    return lpmmp;
}

static void FreeMinMaxParams(LPMINMAXPARAMS lpmmp)
{
    if (!lpmmp)
        return;
    if (lpmmp->hIcon)
        DestroyIcon(lpmmp->hIcon);
    GlobalFree(lpmmp);
}

/* Hook originals */
typedef LRESULT (CALLBACK *DefWindowProcW_t)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *DefWindowProcA_t)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *DefFrameProcW_t)(HWND, HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *DefFrameProcA_t)(HWND, HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *DefMDIChildProcW_t)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *DefMDIChildProcA_t)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *DefDlgProcW_t)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *DefDlgProcA_t)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *ShowWindow_t)(HWND, int);
typedef BOOL (WINAPI *ShowWindowAsync_t)(HWND, int);
typedef BOOL (WINAPI *SystemParametersInfoW_t)(UINT, UINT, PVOID, UINT);
typedef BOOL (WINAPI *SystemParametersInfoA_t)(UINT, UINT, PVOID, UINT);
typedef BOOL (WINAPI *SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);

static DefWindowProcW_t      DefWindowProcW_orig;
static DefWindowProcA_t      DefWindowProcA_orig;
static DefFrameProcW_t       DefFrameProcW_orig;
static DefFrameProcA_t       DefFrameProcA_orig;
static DefMDIChildProcW_t    DefMDIChildProcW_orig;
static DefMDIChildProcA_t    DefMDIChildProcA_orig;
static DefDlgProcW_t         DefDlgProcW_orig;
static DefDlgProcA_t         DefDlgProcA_orig;
static ShowWindow_t          ShowWindow_orig;
static ShowWindowAsync_t     ShowWindowAsync_orig;
static SystemParametersInfoW_t SystemParametersInfoW_orig;
static SystemParametersInfoA_t SystemParametersInfoA_orig;
static SetWindowPos_t        SetWindowPos_orig;

static void AdjustRectForWindowBorder(HWND hwnd, LPRECT prc)
{
    int cBorders = BordersForDpi(
        GetWindowLongPtrW(hwnd, GWL_STYLE),
        GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
        TRUE, FALSE, DpiForWindowOf(hwnd));
    InflateRect(prc, -cBorders, 0);
    OffsetRect(prc, 0, cBorders);
    prc->bottom -= 2 * cBorders;
}

static LRESULT CALLBACK AnimWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == g_uMsgMinMax)
    {
        constexpr UINT ANIMATION_DURATION_MS = 250;
        g_fAnimating = true;

        UINT cmd = (UINT)wParam;
        LPMINMAXPARAMS lpmmp = (LPMINMAXPARAMS)lParam;
        HWND hwndTarget = lpmmp->hwnd;
        TRACE("anim begin cmd=%u", cmd);

        UINT uDpiWnd = DpiForWindowOf(hwndTarget);
        UINT uDpiMon = uDpiWnd;

        if (!IsWindowPerMonitorDpiAware(hwndTarget))
        {
            MONITORINFO mi = { sizeof(mi) };
            HMONITOR hmon = MonitorFromWindow(hwndTarget, MONITOR_DEFAULTTOPRIMARY);
            GetMonitorInfoW(hmon, &mi);
            UINT dpiX = 96, dpiY = 96;
            if (GetDpiForMonitorOf(hmon, &dpiX, &dpiY) == S_OK)
                uDpiMon = dpiX;
            else
                uDpiMon = 96;   /* Win7/8.1：系统指标本身已按 DPI 缩放，取 96 避免二次缩放 */
        }

        bool fAnimate = false;
        DWORD dwStyle = GetWindowLongW(hwndTarget, GWL_STYLE);
        switch (cmd)
        {
            case SC_MAXIMIZE: fAnimate = !(dwStyle & WS_MAXIMIZE); break;
            case SC_MINIMIZE: fAnimate = !(dwStyle & WS_MINIMIZE); break;
            case SC_RESTORE:  fAnimate = (dwStyle & WS_MAXIMIZE) || (dwStyle & WS_MINIMIZE); break;
        }
        if (!fAnimate)
        {
            TRACE("anim skip cmd=%u style=%08lx", cmd, dwStyle);
            g_fAnimating = false;
            FreeMinMaxParams(lpmmp);
            return 0;
        }

        /* Screenshot desktop */
        HDC hdcDesk = GetDC(nullptr);
        HDC hdcMem  = CreateCompatibleDC(hdcDesk);

        int xVirtualScreen  = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int yVirtualScreen  = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int cxVirtualScreen = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int cyVirtualScreen = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        HBITMAP hbmDesk = CreateCompatibleBitmap(hdcDesk, cxVirtualScreen, cyVirtualScreen);
        SelectObject(hdcMem, hbmDesk);
        BitBlt(hdcMem, 0, 0, cxVirtualScreen, cyVirtualScreen,
               hdcDesk, xVirtualScreen, yVirtualScreen, SRCCOPY);

        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcDesk);

        SetWindowPos(hwnd, HWND_TOPMOST, xVirtualScreen, yVirtualScreen,
                     cxVirtualScreen, cyVirtualScreen, SWP_NOACTIVATE);

        RECT rcFrom, rcTo;

        GetWindowRect(hwndTarget, &rcFrom);
        if (MulDiv(rcFrom.left, uDpiWnd, uDpiMon) == -32000)
            GetWindowMinimizeRectForWindowDpi(hwndTarget, &rcFrom);
        AdjustRectForWindowBorder(hwndTarget, &rcFrom);

        switch (cmd)
        {
            case SC_MINIMIZE:
                GetWindowMinimizeRectForWindowDpi(hwndTarget, &rcTo);
                break;

            case SC_MAXIMIZE:
            {
GetMaximizeRect:
                CopyRect(&rcTo, &lpmmp->rcMax);
                if (!IsWindowPerMonitorDpiAware(hwndTarget))
                {
                    MONITORINFO mi = { sizeof(mi) };
                    HMONITOR hmon = MonitorFromWindow(hwndTarget, MONITOR_DEFAULTTOPRIMARY);
                    if (hmon && GetMonitorInfoW(hmon, &mi))
                    {
                        UINT dpiX = 96, dpiY = 96;
                        if (GetDpiForMonitorOf(hmon, &dpiX, &dpiY) != S_OK)
                            dpiX = dpiY = DpiForWindowOf(hwndTarget);   /* 无监视器 DPI API 时按窗口 DPI 原样换算 */
                        UINT dWnd = DpiForWindowOf(hwndTarget);

                        UINT uWidth  = MulDiv(RECTWIDTH(rcTo),  dpiX, dWnd);
                        UINT uHeight = MulDiv(RECTHEIGHT(rcTo), dpiY, dWnd);
                        rcTo.left   = MulDiv(rcTo.left, dpiX, dWnd);
                        rcTo.top    = MulDiv(rcTo.top,  dpiY, dWnd);
                        rcTo.right  = rcTo.left + uWidth;
                        rcTo.bottom = rcTo.top + uHeight;
                    }
                }
                AdjustRectForWindowBorder(hwndTarget, &rcTo);
                break;
            }

            case SC_RESTORE:
            {
                WINDOWPLACEMENT wp = { sizeof(wp) };
                GetWindowPlacement(hwndTarget, &wp);
                if ((dwStyle & WS_MINIMIZE) && (wp.flags & WPF_RESTORETOMAXIMIZED))
                {
                    goto GetMaximizeRect;
                }
                else
                {
                    CopyRect(&rcTo, &wp.rcNormalPosition);
                    if (!(dwStyle & WS_CHILD))
                    {
                        MONITORINFO mi = { sizeof(mi) };
                        HMONITOR hmon = MonitorFromWindow(hwndTarget, MONITOR_DEFAULTTOPRIMARY);
                        if (hmon && GetMonitorInfoW(hmon, &mi))
                        {
                            OffsetRect(&rcTo,
                                -(mi.rcMonitor.left - mi.rcWork.left),
                                -(mi.rcMonitor.top - mi.rcWork.top));
                        }
                    }
                    AdjustRectForWindowBorder(hwndTarget, &rcTo);
                }
                break;
            }
        }

        // MDI child windows: convert child coords to screen coords and clip
        HWND hwndParent;
        HRGN hrgnClip = nullptr;
        RECT rcClip = { 0, 0, cxVirtualScreen, cyVirtualScreen };
        if ((dwStyle & WS_CHILD) && (hwndParent = GetParent(hwndTarget)))
        {
            MapWindowPoints(hwndParent, HWND_DESKTOP, (LPPOINT)&rcTo, 2);

            GetClientRect(hwndParent, &rcClip);
            MapWindowPoints(hwndParent, HWND_DESKTOP, (LPPOINT)&rcClip, 2);
            OffsetRect(&rcClip, -xVirtualScreen, -yVirtualScreen);

            hrgnClip = CreateRectRgnIndirect(&rcClip);

            HWND hwndAbove = GetAncestor(hwndParent, GA_ROOT);
            while ((hwndAbove = GetWindow(hwndAbove, GW_HWNDPREV)) != nullptr)
            {
                if (IsWindowVisible(hwndAbove) && hwndAbove != g_hwndAnim)
                {
                    RECT rcAbove;
                    if (GetWindowRect(hwndAbove, &rcAbove))
                    {
                        RECT rcIntersect;
                        if (IntersectRect(&rcIntersect, &rcAbove, &rcClip))
                        {
                            OffsetRect(&rcAbove, -xVirtualScreen, -yVirtualScreen);
                            HRGN hrgnAbove = CreateRectRgnIndirect(&rcAbove);
                            CombineRgn(hrgnClip, hrgnClip, hrgnAbove, RGN_DIFF);
                            DeleteObject(hrgnAbove);
                        }
                    }
                }
            }
        }

        OffsetRect(&rcFrom, -xVirtualScreen, -yVirtualScreen);
        OffsetRect(&rcTo,   -xVirtualScreen, -yVirtualScreen);

        /* Show window and start the animation */
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        ShowWindow_orig(hwnd, SW_SHOW);

        HDC hdc = GetDC(hwnd);
        hdcDesk = CreateCompatibleDC(hdc);
        SelectObject(hdcDesk, hbmDesk);
        hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdcDesk, cxVirtualScreen, cyVirtualScreen);
        SelectObject(hdcMem, hbmMem);

        BitBlt(hdc, 0, 0, cxVirtualScreen, cyVirtualScreen, hdcDesk, 0, 0, SRCCOPY);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

        UINT64 uStartTime = GetTickCount64();
        UINT64 uElapsedTime = 0;
        RECT rcCurrent;
        CopyRect(&rcCurrent, &rcFrom);

        int cxIcon    = MetricsForDpiOf(SM_CXSMICON, uDpiMon);
        int cyIcon    = MetricsForDpiOf(SM_CYSMICON, uDpiMon);
        int cyCaption = MetricsForDpiOf(SM_CYCAPTION, uDpiMon) - 1;

        int dxIcon = (cyCaption - cxIcon) / 2;
        int dyIcon = (cyCaption - cyIcon) / 2;

        rcCurrent.bottom = rcCurrent.top + cyCaption;

        BOOL fGradient = FALSE;
        SystemParametersInfoW_orig(SPI_GETGRADIENTCAPTIONS, 0, &fGradient, 0);

        bool fHasIcon = (lpmmp->hIcon != nullptr);
        bool fRtl = (GetWindowLongW(hwndTarget, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;

        COLORREF clrCaption         = GetSysColor(COLOR_ACTIVECAPTION);
        COLORREF clrCaptionGradient = GetSysColor(COLOR_GRADIENTACTIVECAPTION);
        COLORREF clrCaptionText     = GetSysColor(COLOR_CAPTIONTEXT);

        HBRUSH hbrCaption = GetSysColorBrush(COLOR_ACTIVECAPTION);

        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SpiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, uDpiMon);
        HFONT hfontCaption = CreateFontIndirectW(&ncm.lfCaptionFont);

        while ((uElapsedTime = GetTickCount64() - uStartTime) <= ANIMATION_DURATION_MS)
        {
            RECT rcOld = rcCurrent;
            rcCurrent.left   = rcFrom.left  + MulDiv(rcTo.left  - rcFrom.left,  (int)uElapsedTime, ANIMATION_DURATION_MS);
            rcCurrent.top    = rcFrom.top   + MulDiv(rcTo.top   - rcFrom.top,   (int)uElapsedTime, ANIMATION_DURATION_MS);
            rcCurrent.right  = rcFrom.right + MulDiv(rcTo.right - rcFrom.right, (int)uElapsedTime, ANIMATION_DURATION_MS);
            rcCurrent.bottom = rcCurrent.top + cyCaption;

            RECT rcDirty;
            rcDirty.left   = MIN2(rcCurrent.left,   rcOld.left);
            rcDirty.top    = MIN2(rcCurrent.top,    rcOld.top);
            rcDirty.right  = MAX2(rcCurrent.right,  rcOld.right);
            rcDirty.bottom = MAX2(rcCurrent.bottom, rcOld.bottom);

            BitBlt(hdcMem,
                   rcDirty.left, rcDirty.top,
                   RECTWIDTH(rcDirty), RECTHEIGHT(rcDirty),
                   hdcDesk, rcDirty.left, rcDirty.top, SRCCOPY);

            SelectClipRgn(hdcMem, hrgnClip);

            if (fGradient)
            {
                int iCaptionOffset = 0;
                if (fHasIcon)
                {
                    RECT rcIconBox;
                    rcIconBox.top    = rcCurrent.top;
                    rcIconBox.bottom = rcCurrent.bottom;
                    if (fRtl)
                    {
                        rcIconBox.left  = rcCurrent.right - cyCaption;
                        rcIconBox.right = rcCurrent.right;
                    }
                    else
                    {
                        rcIconBox.left  = rcCurrent.left;
                        rcIconBox.right = rcCurrent.left + cyCaption;
                    }
                    iCaptionOffset = RECTWIDTH(rcIconBox);
                    FillRect(hdcMem, &rcIconBox, hbrCaption);
                }

                RECT rcGradient = rcCurrent;
                if (fRtl)
                    rcGradient.right -= iCaptionOffset;
                else
                    rcGradient.left += iCaptionOffset;

                COLORREF clrLeft  = fRtl ? clrCaptionGradient : clrCaption;
                COLORREF clrRight = fRtl ? clrCaption : clrCaptionGradient;

                TRIVERTEX vertices[2];
                vertices[0].x     = rcGradient.left;
                vertices[0].y     = rcGradient.top;
                vertices[0].Red   = (COLOR16)(GetRValue(clrLeft) << 8);
                vertices[0].Green = (COLOR16)(GetGValue(clrLeft) << 8);
                vertices[0].Blue  = (COLOR16)(GetBValue(clrLeft) << 8);
                vertices[0].Alpha = 0;
                vertices[1].x     = rcGradient.right;
                vertices[1].y     = rcGradient.bottom;
                vertices[1].Red   = (COLOR16)(GetRValue(clrRight) << 8);
                vertices[1].Green = (COLOR16)(GetGValue(clrRight) << 8);
                vertices[1].Blue  = (COLOR16)(GetBValue(clrRight) << 8);
                vertices[1].Alpha = 0;

                GRADIENT_RECT mesh = { 1, 0 };
                GdiGradientFill(hdcMem, vertices, ARRAYSIZE(vertices), &mesh, 1, GRADIENT_FILL_RECT_H);
            }
            else
            {
                FillRect(hdcMem, &rcCurrent, hbrCaption);
            }

            if (fHasIcon)
            {
                int xIcon = fRtl ? (rcCurrent.right - dxIcon - cxIcon) : (rcCurrent.left + dxIcon);
                int yIcon = rcCurrent.top + dyIcon;
                DrawIconEx(hdcMem, xIcon, yIcon, lpmmp->hIcon,
                           cxIcon, cyIcon, 0, hbrCaption, DI_NORMAL);
            }

            if (lpmmp->szCaptionText[0])
            {
                RECT rcText;
                CopyRect(&rcText, &rcCurrent);
                if (fRtl)
                    rcText.right -= 1 + (fHasIcon ? cyCaption : 0);
                else
                    rcText.left  += 1 + (fHasIcon ? cyCaption : 0);

                HFONT hfontOld = (HFONT)SelectObject(hdcMem, hfontCaption);
                COLORREF clrOld = SetTextColor(hdcMem, clrCaptionText);
                int iBkOld = SetBkMode(hdcMem, TRANSPARENT);
                UINT uFormat = DT_SINGLELINE | DT_VCENTER;
                if (fRtl)
                    uFormat |= DT_RIGHT;

                DrawTextW(hdcMem, lpmmp->szCaptionText, -1, &rcText, uFormat);

                SelectObject(hdcMem, hfontOld);
                SetTextColor(hdcMem, clrOld);
                SetBkMode(hdcMem, iBkOld);
            }

            SelectClipRgn(hdcMem, nullptr);

            BitBlt(hdc,
                   rcDirty.left, rcDirty.top,
                   RECTWIDTH(rcDirty), RECTHEIGHT(rcDirty),
                   hdcMem, rcDirty.left, rcDirty.top, SRCCOPY);
        }

        if (hrgnClip)
            DeleteObject(hrgnClip);
        DeleteDC(hdcDesk);
        DeleteDC(hdcMem);
        DeleteObject(hbmMem);
        DeleteObject(hbmDesk);
        DeleteObject(hfontCaption);
        FreeMinMaxParams(lpmmp);

        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE);
        ShowWindow_orig(hwnd, SW_HIDE);
        g_fAnimating = false;
        TRACE("anim end cmd=%u elapsed=%llu", cmd, uElapsedTime);
        return 0;
    }

    switch (uMsg)
    {
        case WM_PAINT:
        case WM_ERASEBKGND:
            return 0;
        case WM_NCHITTEST:
            return HTNOWHERE;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW_orig ? DefWindowProcW_orig(hwnd, uMsg, wParam, lParam)
                                       : DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
}

static LPCWSTR c_szAnimClassName = L"ClassicMinMax_AnimWindow";

static DWORD CALLBACK AnimWndThreadProc(LPVOID lpParam)
{
    HANDLE hEvent = (HANDLE)lpParam;
    if (pSetThreadDpiAwarenessContext)
        pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc   = AnimWndProc;
    wc.hInstance     = g_hinst;
    wc.lpszClassName = c_szAnimClassName;
    RegisterClassW(&wc);

    g_hwndAnim = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        c_szAnimClassName, nullptr,
        WS_VISIBLE | WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, g_hinst, nullptr);

    if (g_hwndAnim)
    {
        LONG_PTR ex = GetWindowLongPtrW(g_hwndAnim, GWL_EXSTYLE);
        ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
        SetWindowLongPtrW(g_hwndAnim, GWL_EXSTYLE, ex);
    }

    g_uMsgMinMax = RegisterWindowMessageW(L"ClassicMinMax_MinMax");
    SetEvent(hEvent);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static bool WaitForAnimWndThread()
{
    if (!g_hAnimWndThread)
    {
        HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_hAnimWndThread = CreateThread(nullptr, 0, AnimWndThreadProc, hEvent, 0, nullptr);
        DWORD dwWait = WaitForSingleObject(hEvent, 1000);
        if (dwWait != WAIT_OBJECT_0)
        {
            g_fDisabled = true;
            CloseHandle(hEvent);
            return false;
        }
        CloseHandle(hEvent);
        Sleep(30);
    }
    return true;
}

static void MinMaxWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    if (g_fDisabled)
        return;

    if (uMsg != WM_SYSCOMMAND)
        return;

    UINT cmd = (UINT)(wParam & 0xFFF0);
    if (cmd != SC_MINIMIZE && cmd != SC_MAXIMIZE && cmd != SC_RESTORE)
        return;

    TRACE("mm cmd=%u hwnd=%p style=%08lx vis=%d en=%d anim=%d", cmd, hwnd,
          GetWindowLongW(hwnd, GWL_STYLE), IsWindowVisible(hwnd), IsEnabled(), g_fAnimating);

    if (!IsEnabled() || !WaitForAnimWndThread() || g_fAnimating)
        return;

    LPMINMAXPARAMS lpmmp = CreateMinMaxParams(hwnd, cmd);
    if (!lpmmp)
        return;

    SendMessageW(g_hwndAnim, g_uMsgMinMax, cmd, (LPARAM)lpmmp);
}

/* Hooks */
static LRESULT CALLBACK DefWindowProcW_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefWindowProcW_orig(hWnd, uMsg, wParam, lParam);
}
static LRESULT CALLBACK DefWindowProcA_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefWindowProcA_orig(hWnd, uMsg, wParam, lParam);
}
static LRESULT CALLBACK DefFrameProcW_hook(HWND hWnd, HWND hWndMDIClient, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefFrameProcW_orig(hWnd, hWndMDIClient, uMsg, wParam, lParam);
}
static LRESULT CALLBACK DefFrameProcA_hook(HWND hWnd, HWND hWndMDIClient, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefFrameProcA_orig(hWnd, hWndMDIClient, uMsg, wParam, lParam);
}
static LRESULT CALLBACK DefMDIChildProcW_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefMDIChildProcW_orig(hWnd, uMsg, wParam, lParam);
}
static LRESULT CALLBACK DefMDIChildProcA_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefMDIChildProcA_orig(hWnd, uMsg, wParam, lParam);
}
static LRESULT CALLBACK DefDlgProcW_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefDlgProcW_orig(hWnd, uMsg, wParam, lParam);
}
static LRESULT CALLBACK DefDlgProcA_hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MinMaxWndProc(hWnd, uMsg, wParam, lParam);
    return DefDlgProcA_orig(hWnd, uMsg, wParam, lParam);
}

static BOOL WINAPI ShowWindow_hook(HWND hWnd, int nCmdShow)
{
    if (!IsEnabled() || g_fDisabled || g_fAnimating || !IsWindowVisible(hWnd))
        return ShowWindow_orig(hWnd, nCmdShow);

    DWORD dwStyle = GetWindowLongW(hWnd, GWL_STYLE);
    if (!(dwStyle & WS_CAPTION))
        return ShowWindow_orig(hWnd, nCmdShow);

    UINT cmd;
    switch (nCmdShow)
    {
        case SW_MAXIMIZE: cmd = SC_MAXIMIZE; break;
        case SW_MINIMIZE: cmd = SC_MINIMIZE; break;
        case SW_RESTORE:  cmd = SC_RESTORE;  break;
        default: return ShowWindow_orig(hWnd, nCmdShow);
    }

    TRACE("sw nCmd=%d hwnd=%p style=%08lx", nCmdShow, hWnd, dwStyle);

    if (!WaitForAnimWndThread())
        return ShowWindow_orig(hWnd, nCmdShow);

    LPMINMAXPARAMS lpmmp = CreateMinMaxParams(hWnd, cmd);
    if (!lpmmp)
        return ShowWindow_orig(hWnd, nCmdShow);

    SendMessageW(g_hwndAnim, g_uMsgMinMax, cmd, (LPARAM)lpmmp);
    return ShowWindow_orig(hWnd, nCmdShow);
}

static BOOL WINAPI ShowWindowAsync_hook(HWND hWnd, int nCmdShow)
{
    if (!IsEnabled() || g_fDisabled || g_fAnimating || !IsWindowVisible(hWnd))
        return ShowWindowAsync_orig(hWnd, nCmdShow);

    DWORD dwStyle = GetWindowLongW(hWnd, GWL_STYLE);
    if (!(dwStyle & WS_CAPTION))
        return ShowWindowAsync_orig(hWnd, nCmdShow);

    UINT cmd;
    switch (nCmdShow)
    {
        case SW_MAXIMIZE: cmd = SC_MAXIMIZE; break;
        case SW_MINIMIZE: cmd = SC_MINIMIZE; break;
        case SW_RESTORE:  cmd = SC_RESTORE;  break;
        default: return ShowWindowAsync_orig(hWnd, nCmdShow);
    }

    if (!WaitForAnimWndThread())
        return ShowWindowAsync_orig(hWnd, nCmdShow);

    LPMINMAXPARAMS lpmmp = CreateMinMaxParams(hWnd, cmd);
    if (!lpmmp)
        return ShowWindowAsync_orig(hWnd, nCmdShow);

    SendMessageW(g_hwndAnim, g_uMsgMinMax, cmd, (LPARAM)lpmmp);
    return ShowWindowAsync_orig(hWnd, nCmdShow);
}

static BOOL WINAPI SetWindowPos_hook(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
    Sleep(1);
    return SetWindowPos_orig(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

static bool MinMaxSystemParametersInfo(UINT uiAction, UINT uiParam, PVOID pvParam, BOOL* pfResult)
{
    if (!IsEnabled())
        return false;

    switch (uiAction)
    {
        case SPI_GETANIMATION:
        {
            LPANIMATIONINFO lpai = (LPANIMATIONINFO)pvParam;
            if (!lpai || uiParam != sizeof(*lpai) || lpai->cbSize != sizeof(*lpai))
            {
                *pfResult = FALSE;
                return true;
            }
            lpai->cbSize = sizeof(*lpai);
            // 系统级 MinAnimate 已被宿主关闭，这里对应用谎称动画仍开启
            lpai->iMinAnimate = 1;
            *pfResult = TRUE;
            return true;
        }
        case SPI_SETANIMATION:
        {
            LPANIMATIONINFO lpai = (LPANIMATIONINFO)pvParam;
            if (!lpai || lpai->cbSize != sizeof(*lpai))
            {
                *pfResult = FALSE;
                return true;
            }
            *pfResult = TRUE;
            return true;
        }
        default:
            return false;
    }
}

static BOOL WINAPI SystemParametersInfoW_hook(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
{
    BOOL fResult;
    if (MinMaxSystemParametersInfo(uiAction, uiParam, pvParam, &fResult))
        return fResult;
    return SystemParametersInfoW_orig(uiAction, uiParam, pvParam, fWinIni);
}

static BOOL WINAPI SystemParametersInfoA_hook(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
{
    BOOL fResult;
    if (MinMaxSystemParametersInfo(uiAction, uiParam, pvParam, &fResult))
        return fResult;
    return SystemParametersInfoA_orig(uiAction, uiParam, pvParam, fWinIni);
}

/* Init */
static void* GetProc(HMODULE h, const char* name)
{
    return h ? (void*)GetProcAddress(h, name) : nullptr;
}

static bool HookFn(const char* name, void* target, void* hook, void** orig)
{
    if (!target)
    {
        DbgLog("hook %s: target not found", name);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, hook, orig);
    if (st != MH_OK)
    {
        DbgLog("hook %s: MH_CreateHook failed %d", name, (int)st);
        return false;
    }
    return true;
}

static void InitThread()
{
    g_fDebugTrace = RegGetDword(L"Debug", 0) != 0;

    /* 开机自启时，宿主可能在目标进程刚创建、user32 还没加载完时就注入进来了。
       这里最多等 10 秒再装钩子，避免出现“注入成功但一直没动画”。 */
    HMODULE hUser = nullptr;
    for (int i = 0; i < 100; i++)
    {
        hUser = GetModuleHandleW(L"user32.dll");
        if (hUser)
            break;
        Sleep(100);
    }
    if (!hUser)
    {
        DbgLog("no user32, hooks skipped pid=%lu", GetCurrentProcessId());
        return;
    }

    pAdjustWindowRectExForDpi = (AdjustWindowRectExForDpi_t)GetProc(hUser, "AdjustWindowRectExForDpi");
    pGetWindowMinimizeRect   = (GetWindowMinimizeRect_t)GetProc(hUser, "GetWindowMinimizeRect");
    pInternalGetWindowIcon   = (InternalGetWindowIcon_t)GetProc(hUser, "InternalGetWindowIcon");

    /* Win10 DPI API 动态解析；Win7/8.1 上取不到就走降级路径 */
    pGetWindowDpiAwarenessContext = (GetWindowDpiAwarenessContext_t)GetProc(hUser, "GetWindowDpiAwarenessContext");
    pAreDpiAwarenessContextsEqual = (AreDpiAwarenessContextsEqual_t)GetProc(hUser, "AreDpiAwarenessContextsEqual");
    pGetDpiForWindow              = (GetDpiForWindow_t)GetProc(hUser, "GetDpiForWindow");
    pGetSystemMetricsForDpi       = (GetSystemMetricsForDpi_t)GetProc(hUser, "GetSystemMetricsForDpi");
    pSystemParametersInfoForDpi   = (SystemParametersInfoForDpi_t)GetProc(hUser, "SystemParametersInfoForDpi");
    pSetThreadDpiAwarenessContext = (SetThreadDpiAwarenessContext_t)GetProc(hUser, "SetThreadDpiAwarenessContext");

    HMODULE hShcore = LoadLibraryW(L"shcore.dll");   /* Win8.1+ 才有 */
    if (hShcore)
        pGetDpiForMonitor = (GetDpiForMonitor_t)GetProcAddress(hShcore, "GetDpiForMonitor");

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK)
    {
        DbgLog("MH_Initialize failed %d", (int)mh);
        g_fDisabled = true;
        return;
    }
    HookFn("DefWindowProcW",   GetProc(hUser, "DefWindowProcW"),   (void*)DefWindowProcW_hook,   (void**)&DefWindowProcW_orig);
    HookFn("DefWindowProcA",   GetProc(hUser, "DefWindowProcA"),   (void*)DefWindowProcA_hook,   (void**)&DefWindowProcA_orig);
    HookFn("DefFrameProcW",    GetProc(hUser, "DefFrameProcW"),    (void*)DefFrameProcW_hook,    (void**)&DefFrameProcW_orig);
    HookFn("DefFrameProcA",    GetProc(hUser, "DefFrameProcA"),    (void*)DefFrameProcA_hook,    (void**)&DefFrameProcA_orig);
    HookFn("DefMDIChildProcW", GetProc(hUser, "DefMDIChildProcW"), (void*)DefMDIChildProcW_hook, (void**)&DefMDIChildProcW_orig);
    HookFn("DefMDIChildProcA", GetProc(hUser, "DefMDIChildProcA"), (void*)DefMDIChildProcA_hook, (void**)&DefMDIChildProcA_orig);
    HookFn("DefDlgProcW",      GetProc(hUser, "DefDlgProcW"),      (void*)DefDlgProcW_hook,      (void**)&DefDlgProcW_orig);
    HookFn("DefDlgProcA",      GetProc(hUser, "DefDlgProcA"),      (void*)DefDlgProcA_hook,      (void**)&DefDlgProcA_orig);
    HookFn("ShowWindow",       GetProc(hUser, "ShowWindow"),       (void*)ShowWindow_hook,       (void**)&ShowWindow_orig);
    HookFn("ShowWindowAsync",  GetProc(hUser, "ShowWindowAsync"),  (void*)ShowWindowAsync_hook,  (void**)&ShowWindowAsync_orig);
    HookFn("SystemParametersInfoW", GetProc(hUser, "SystemParametersInfoW"), (void*)SystemParametersInfoW_hook, (void**)&SystemParametersInfoW_orig);
    HookFn("SystemParametersInfoA", GetProc(hUser, "SystemParametersInfoA"), (void*)SystemParametersInfoA_hook, (void**)&SystemParametersInfoA_orig);

    // Explorer-only mitigation for the random topmost bug
    if (TopmostFixOn())
    {
        WCHAR szExplorer[MAX_PATH], szSelf[MAX_PATH];
        ExpandEnvironmentStringsW(L"%SystemRoot%\\explorer.exe", szExplorer, ARRAYSIZE(szExplorer));
        GetModuleFileNameW(nullptr, szSelf, ARRAYSIZE(szSelf));
        if (_wcsicmp(szSelf, szExplorer) == 0)
        {
            HMODULE hmodTemp = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"ExplorerEx.dll", &hmodTemp) || !hmodTemp)
                HookFn("SetWindowPos", GetProc(hUser, "SetWindowPos"), (void*)SetWindowPos_hook, (void**)&SetWindowPos_orig);
        }
    }

    if (!DefWindowProcW_orig && !DefWindowProcA_orig)
    {
        DbgLog("hook install failed pid=%lu", GetCurrentProcessId());
        g_fDisabled = true;
        return;
    }

    MH_EnableHook(MH_ALL_HOOKS);

    /* 钩子已生效标记，宿主用它确认注入完成（同名事件按进程 PID 区分） */
    WCHAR szEvent[64];
    _snwprintf_s(szEvent, _TRUNCATE, L"Local\\ClassicMinMax_Hooked_%lu", GetCurrentProcessId());
    CreateEventW(nullptr, TRUE, FALSE, szEvent);
}

static DWORD WINAPI InitThreadEntry(LPVOID)
{
    InitThread();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        g_hinst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        HANDLE h = CreateThread(nullptr, 0, InitThreadEntry, nullptr, 0, nullptr);
        if (h)
            CloseHandle(h);
    }
    return TRUE;
}


