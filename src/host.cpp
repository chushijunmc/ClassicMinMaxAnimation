// ClassicMinMax host: tray UI, DLL injector, autostart, global on/off.
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <set>
#include <map>
#include <vector>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

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

static const wchar_t* kRegKey     = L"Software\\ClassicMinMax";
static const wchar_t* kTaskName   = L"ClassicMinMax";
static const wchar_t* kMutexName  = L"Global\\ClassicMinMaxHost";
static const UINT     WM_TRAYICON = WM_APP + 1;

static const int IDM_ENABLE    = 1001;
static const int IDM_AUTOSTART = 1002;
static const int IDM_EXIT      = 1003;

/* 资源里的程序图标 ID（见 src/app.rc） */
#define IDI_APPICON 101

static HINSTANCE g_hinst      = nullptr;
static HWND      g_hwnd       = nullptr;
static HANDLE    g_hMonitor   = nullptr;
static volatile bool g_bMonitorRun = false;
static std::wstring g_exeDir;
static DWORD     g_origMinAnimate = 1;
static UINT      g_msgTaskbarCreated = 0;

/* ---------------- settings ---------------- */

static DWORD RegGetDword(const wchar_t* name, DWORD def)
{
    DWORD v = def, cb = sizeof(v), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, &type, &v, &cb) != ERROR_SUCCESS)
        return def;
    return v;
}

static void RegSetDword(const wchar_t* name, DWORD val)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);
}

static bool IsEnabled()     { return RegGetDword(L"Enabled", 1) != 0; }
static void SetEnabled(bool on) { RegSetDword(L"Enabled", on ? 1 : 0); }

/* ---------------- system animation ---------------- */

static void SetSystemMinAnimate(BOOL on)
{
    ANIMATIONINFO ai;
    ai.cbSize = sizeof(ai);
    ai.iMinAnimate = on;
    SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0);
}

static void ReadOriginalAnimation()
{
    ANIMATIONINFO ai;
    ai.cbSize = sizeof(ai);
    ai.iMinAnimate = 1;
    if (!SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0))
        ai.iMinAnimate = 1;
    DWORD saved = RegGetDword(L"OrigMinAnimate", 0xFFFFFFFF);
    if (saved == 0xFFFFFFFF)
        RegSetDword(L"OrigMinAnimate", ai.iMinAnimate ? 1 : 0);
    g_origMinAnimate = RegGetDword(L"OrigMinAnimate", 1);
}

/* 登录过程中系统可能把“开启动画”重新写回，启用状态下持续校正 */
static void EnsureSystemAnimationOff()
{
    ANIMATIONINFO ai = { sizeof(ai) };
    if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0) && ai.iMinAnimate)
        SetSystemMinAnimate(FALSE);
}

static void RestoreSystemAnimation()
{
    if (g_origMinAnimate)
        SetSystemMinAnimate(TRUE);
    RegSetDword(L"OrigMinAnimate", 0xFFFFFFFF);
}

/* ---------------- elevation ---------------- */

static bool IsElevated()
{
    HANDLE hToken = nullptr;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION te = {};
        DWORD cb = sizeof(te);
        if (GetTokenInformation(hToken, TokenElevation, &te, cb, &cb))
            elevated = te.TokenIsElevated != 0;
        CloseHandle(hToken);
    }
    return elevated;
}

static void RelaunchElevated()
{
    WCHAR szPath[MAX_PATH];
    GetModuleFileNameW(nullptr, szPath, ARRAYSIZE(szPath));
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb  = L"runas";
    sei.lpFile  = szPath;
    sei.nShow   = SW_SHOWNORMAL;
    sei.fMask   = SEE_MASK_NOCLOSEPROCESS;
    if (ShellExecuteExW(&sei) && sei.hProcess)
        CloseHandle(sei.hProcess);
}

/* ---------------- helpers ---------------- */

static DWORD RunHidden(const std::wstring& cmd)
{
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, &mut[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static bool IsAutostartEnabled()
{
    std::wstring q = L"cmd";
    (void)q;
    std::wstring cmd = L"schtasks.exe /Query /TN \"";
    cmd += kTaskName;
    cmd += L"\"";
    return RunHidden(cmd) == 0;
}

static void SetAutostart(bool on)
{
    std::wstring exe;
    WCHAR szPath[MAX_PATH];
    GetModuleFileNameW(nullptr, szPath, ARRAYSIZE(szPath));
    exe = szPath;

    if (on)
    {
        std::wstring cmd = L"schtasks.exe /Create /TN \"";
        cmd += kTaskName;
        cmd += L"\" /TR \"\\\"";
        cmd += exe;
        cmd += L"\\\"\" /SC ONLOGON /RL HIGHEST /F";
        RunHidden(cmd);
    }
    else
    {
        std::wstring cmd = L"schtasks.exe /Delete /TN \"";
        cmd += kTaskName;
        cmd += L"\" /F";
        RunHidden(cmd);
    }
}

/* ---------------- remote PE export resolution ---------------- */

static ULONGLONG GetRemoteModuleBase(DWORD pid, const wchar_t* dllName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32W me = { sizeof(me) };
    ULONGLONG base = 0;
    if (Module32FirstW(snap, &me))
    {
        do
        {
            if (_wcsicmp(me.szModule, dllName) == 0)
            {
                base = (ULONGLONG)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

static ULONGLONG RemoteProcAddress(HANDLE hProc, DWORD pid, ULONGLONG base, const char* procName, int depth)
{
    if (depth > 4)
        return 0;

    IMAGE_DOS_HEADER dos = {};
    if (!ReadProcessMemory(hProc, (LPCVOID)base, &dos, sizeof(dos), nullptr))
        return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    DWORD sig = 0;
    if (!ReadProcessMemory(hProc, (LPCVOID)(base + dos.e_lfanew), &sig, sizeof(sig), nullptr))
        return 0;
    if (sig != IMAGE_NT_SIGNATURE)
        return 0;

    IMAGE_FILE_HEADER fh = {};
    ULONGLONG fhAddr = base + dos.e_lfanew + 4;
    if (!ReadProcessMemory(hProc, (LPCVOID)fhAddr, &fh, sizeof(fh), nullptr))
        return 0;

    WORD magic = 0;
    ULONGLONG optAddr = fhAddr + sizeof(fh);
    if (!ReadProcessMemory(hProc, (LPCVOID)optAddr, &magic, sizeof(magic), nullptr))
        return 0;

    DWORD exportRva = 0, exportSize = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        IMAGE_OPTIONAL_HEADER64 opt = {};
        if (!ReadProcessMemory(hProc, (LPCVOID)optAddr, &opt, sizeof(opt), nullptr))
            return 0;
        exportRva  = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        exportSize = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    }
    else
    {
        IMAGE_OPTIONAL_HEADER32 opt = {};
        if (!ReadProcessMemory(hProc, (LPCVOID)optAddr, &opt, sizeof(opt), nullptr))
            return 0;
        exportRva  = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        exportSize = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    }
    if (!exportRva)
        return 0;

    IMAGE_EXPORT_DIRECTORY exp = {};
    if (!ReadProcessMemory(hProc, (LPCVOID)(base + exportRva), &exp, sizeof(exp), nullptr))
        return 0;

    for (DWORD i = 0; i < exp.NumberOfNames; i++)
    {
        DWORD nameRva = 0;
        if (!ReadProcessMemory(hProc, (LPCVOID)(base + exp.AddressOfNames + i * 4), &nameRva, sizeof(nameRva), nullptr))
            continue;
        char name[64] = {};
        if (!ReadProcessMemory(hProc, (LPCVOID)(base + nameRva), name, sizeof(name) - 1, nullptr))
            continue;
        if (strcmp(name, procName) != 0)
            continue;

        WORD ord = 0;
        if (!ReadProcessMemory(hProc, (LPCVOID)(base + exp.AddressOfNameOrdinals + i * 2), &ord, sizeof(ord), nullptr))
            return 0;
        DWORD funcRva = 0;
        if (!ReadProcessMemory(hProc, (LPCVOID)(base + exp.AddressOfFunctions + ord * 4), &funcRva, sizeof(funcRva), nullptr))
            return 0;
        if (!funcRva)
            return 0;

        // Forwarded export: RVA points inside the export directory
        if (exportSize && funcRva >= exportRva && funcRva < exportRva + exportSize)
        {
            char fwd[128] = {};
            if (!ReadProcessMemory(hProc, (LPCVOID)(base + funcRva), fwd, sizeof(fwd) - 1, nullptr))
                return 0;
            char* dot = strchr(fwd, '.');
            if (!dot)
                return 0;
            *dot = '\0';
            std::wstring modName;
            for (char* p = fwd; *p; p++)
                modName += (wchar_t)*p;
            modName += L".dll";
            ULONGLONG base2 = GetRemoteModuleBase(pid, modName.c_str());
            if (!base2)
                return 0;
            return RemoteProcAddress(hProc, pid, base2, dot + 1, depth + 1);
        }
        return base + funcRva;
    }
    return 0;
}

/* ---------------- injection ---------------- */

static bool Is32BitProcess(HANDLE hProc)
{
    typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT*, USHORT*);
    static IsWow64Process2_t pIsWow64Process2 =
        (IsWow64Process2_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    if (pIsWow64Process2)
    {
        USHORT procMachine = 0, nativeMachine = 0;
        if (pIsWow64Process2(hProc, &procMachine, &nativeMachine))
            return procMachine == IMAGE_FILE_MACHINE_I386;
    }
    BOOL bWow = FALSE;
    if (IsWow64Process(hProc, &bWow))
        return bWow != FALSE;
    return false;
}

static bool IsModuleLoaded(DWORD pid, const wchar_t* moduleName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me))
    {
        do
        {
            if (_wcsicmp(me.szModule, moduleName) == 0)
            {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool InjectDll(DWORD pid, const std::wstring& dllPath)
{
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
    if (!hProc)
        return false;

    bool ok = false;
    DWORD exitCode = 0;
    ULONGLONG k32 = GetRemoteModuleBase(pid, L"kernel32.dll");
    if (!k32)
        DbgLog("  k32 base not found");
    ULONGLONG loadLib = (k32 ? RemoteProcAddress(hProc, pid, k32, "LoadLibraryW", 0) : 0);
    if (!loadLib)
        DbgLog("  LoadLibraryW addr not found");
    if (loadLib)
    {
        SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote)
            DbgLog("  VirtualAllocEx failed %lu", GetLastError());
        if (remote && WriteProcessMemory(hProc, remote, dllPath.c_str(), bytes, nullptr))
        {
            HANDLE th = CreateRemoteThread(hProc, nullptr, 0,
                                           (LPTHREAD_START_ROUTINE)(ULONG_PTR)loadLib, remote, 0, nullptr);
            if (!th)
                DbgLog("  CreateRemoteThread failed %lu", GetLastError());
            if (th)
            {
                WaitForSingleObject(th, 5000);
                GetExitCodeThread(th, &exitCode);
                CloseHandle(th);
            }
            VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

            const wchar_t* slash = wcsrchr(dllPath.c_str(), L'\\');
            const wchar_t* dllName = slash ? slash + 1 : dllPath.c_str();
            ok = exitCode != 0 || IsModuleLoaded(pid, dllName);
            if (!ok)
                DbgLog("  inject failed pid=%lu exit=%lu", pid, exitCode);
        }
        else if (remote)
        {
            DbgLog("  WriteProcessMemory failed %lu", GetLastError());
        }
    }
    CloseHandle(hProc);
    return ok;
}

static bool ShouldSkip(const wchar_t* name, DWORD pid)
{
    static const wchar_t* blacklist[] = {
        L"system", L"idle", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe", L"svchost.exe",
        L"dwm.exe", L"fontdrvhost.exe", L"audiodg.exe", L"MsMpEng.exe", L"SecurityHealthService.exe",
        L"MemCompression", L"WUDFHost.exe", L"WmiPrvSE.exe", L"spoolsv.exe", L"Register.exe",
    };
    if (pid == GetCurrentProcessId())
        return true;
    for (const wchar_t* b : blacklist)
    {
        if (_wcsicmp(name, b) == 0)
            return true;
    }
    return false;
}

static CRITICAL_SECTION g_cs;
static std::set<DWORD> g_injected;
static std::map<DWORD, int> g_attempts;

static bool AlreadyInjected(DWORD pid)
{
    EnterCriticalSection(&g_cs);
    bool found = g_injected.count(pid) != 0;
    LeaveCriticalSection(&g_cs);
    return found;
}

static void MarkInjected(DWORD pid)
{
    EnterCriticalSection(&g_cs);
    g_injected.insert(pid);
    LeaveCriticalSection(&g_cs);
}

static void PruneInjected()
{
    EnterCriticalSection(&g_cs);
    for (auto it = g_injected.begin(); it != g_injected.end();)
    {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, *it);
        bool dead = true;
        if (h)
        {
            dead = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
            CloseHandle(h);
        }
        if (dead)
        {
            g_attempts.erase(*it);
            it = g_injected.erase(it);
        }
        else
            ++it;
    }
    LeaveCriticalSection(&g_cs);
}

/* DLL 装好钩子后会创建 Local\ClassicMinMax_Hooked_<pid> 事件。
   宿主用它确认“注入真正生效”，没生效就下一轮扫描再试，
   避免开机自启时进程刚创建、DLL 还没装好钩子就被标记为已完成。 */
static bool HooksReady(DWORD pid)
{
    WCHAR name[64];
    _snwprintf_s(name, _TRUNCATE, L"Local\\ClassicMinMax_Hooked_%lu", pid);
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (ev)
        CloseHandle(ev);
    return ev != nullptr;
}

static bool IsAppContainerProcess(HANDLE hProc)
{
    HANDLE hTok = nullptr;
    bool appc = false;
    if (OpenProcessToken(hProc, TOKEN_QUERY, &hTok))
    {
        DWORD isApp = 0, cb = 0;
        if (GetTokenInformation(hTok, TokenIsAppContainer, &isApp, sizeof(isApp), &cb))
            appc = isApp != 0;
        CloseHandle(hTok);
    }
    return appc;
}

static void ScanAndInject()
{
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (pe.th32ProcessID == 0 || ShouldSkip(pe.szExeFile, pe.th32ProcessID))
                continue;

            DWORD session = 0;
            if (ProcessIdToSessionId(pe.th32ProcessID, &session) && session != mySession)
                continue;

            if (AlreadyInjected(pe.th32ProcessID))
                continue;

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProc)
                continue;

            if (IsAppContainerProcess(hProc))
            {
                CloseHandle(hProc);
                continue;
            }

            bool is32 = Is32BitProcess(hProc);
            CloseHandle(hProc);

            std::wstring dll = g_exeDir + (is32 ? L"\\MinMaxHook32.dll" : L"\\MinMaxHook64.dll");
            bool injected = InjectDll(pe.th32ProcessID, dll);

            /* 注入成功且钩子确实已装好才算完成；否则下一轮重试（约 12 秒后放弃） */
            int attempts = 0;
            EnterCriticalSection(&g_cs);
            attempts = ++g_attempts[pe.th32ProcessID];
            LeaveCriticalSection(&g_cs);

            if ((injected && HooksReady(pe.th32ProcessID)) || attempts >= 12)
                MarkInjected(pe.th32ProcessID);
            else
                DbgLog("  hooks pending pid=%lu injected=%d attempt=%d", pe.th32ProcessID, injected ? 1 : 0, attempts);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    DbgLog("CRASH code=%08lx addr=%p", ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI MonitorThread(LPVOID)
{
    ULONGLONG lastPrune = GetTickCount64();
    while (g_bMonitorRun)
    {
        __try { ScanAndInject(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DbgLog("scan exception %08lx", GetExceptionCode());
        }
        if (IsEnabled())
            EnsureSystemAnimationOff();
        if (GetTickCount64() - lastPrune > 10000)
        {
            PruneInjected();
            lastPrune = GetTickCount64();
        }
        for (int i = 0; i < 10 && g_bMonitorRun; i++)
            Sleep(100);
    }
    return 0;
}

static void StartMonitor()
{
    if (g_bMonitorRun)
        return;
    g_bMonitorRun = true;
    g_hMonitor = CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr);
}

static void StopMonitor()
{
    if (!g_bMonitorRun)
        return;
    g_bMonitorRun = false;
    if (g_hMonitor)
    {
        WaitForSingleObject(g_hMonitor, 3000);
        CloseHandle(g_hMonitor);
        g_hMonitor = nullptr;
    }
}

/* ---------------- tray ---------------- */

static void UpdateTrayTip()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcscpy_s(nid.szTip, IsEnabled() ? L"经典最小化/最大化动画：已启用"
                                    : L"经典最小化/最大化动画：已禁用");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void AddTrayIcon()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    if (!nid.hIcon)
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, IsEnabled() ? L"经典最小化/最大化动画：已启用"
                                    : L"经典最小化/最大化动画：已禁用");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ToggleEnabled(bool on)
{
    SetEnabled(on);
    if (on)
    {
        SetSystemMinAnimate(FALSE);
        StartMonitor();
    }
    else
    {
        StopMonitor();
        SetSystemMinAnimate(g_origMinAnimate ? TRUE : FALSE);
    }
    UpdateTrayTip();
}

static void ShowTrayMenu()
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (IsEnabled() ? MF_CHECKED : 0), IDM_ENABLE,
                L"启用经典最小化/最大化动画");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART,
                L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

static void ExitApp()
{
    DbgLog("exit app");
    StopMonitor();
    RemoveTrayIcon();
    RestoreSystemAnimation();
    DestroyWindow(g_hwnd);
}

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_TRAYICON)
    {
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP)
            ShowTrayMenu();
        return 0;
    }

    if (uMsg == g_msgTaskbarCreated && g_msgTaskbarCreated != 0)
    {
        AddTrayIcon();
        return 0;
    }

    switch (uMsg)
    {
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDM_ENABLE:    ToggleEnabled(!IsEnabled()); break;
                case IDM_AUTOSTART: SetAutostart(!IsAutostartEnabled()); break;
                case IDM_EXIT:      ExitApp(); break;
            }
            return 0;

        case WM_ENDSESSION:
            DbgLog("wm endsession");
            ExitApp();
            return 0;

        case WM_CLOSE:
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

/* Win10 1703+ 才导出，Win7/8.1 必须动态解析，否则进程启动即报“找不到入口点” */
static void EnableDpiAwareness()
{
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
    typedef BOOL (WINAPI *SetProcessDPIAware_t)();

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    SetProcessDpiAwarenessContext_t pCtx = hUser
        ? (SetProcessDpiAwarenessContext_t)GetProcAddress(hUser, "SetProcessDpiAwarenessContext")
        : nullptr;
    if (pCtx && pCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return;

    /* Win7/8.1 回退：系统级 DPI 感知（Vista+） */
    SetProcessDPIAware_t pOld = hUser
        ? (SetProcessDPIAware_t)GetProcAddress(hUser, "SetProcessDPIAware")
        : nullptr;
    if (pOld)
        pOld();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nShowCmd)
{
    (void)hPrev; (void)lpCmdLine; (void)nShowCmd;

    EnableDpiAwareness();
    SetUnhandledExceptionFilter(CrashFilter);

    bool elevated = IsElevated();
    DbgLog("wWinMain start elevated=%d", (int)elevated);

    if (!elevated)
    {
        DbgLog("relaunch elevated");
        RelaunchElevated();
        return 0;
    }

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        DbgLog("already running, exit");
        CloseHandle(hMutex);
        return 0;
    }

    g_hinst = hInst;

    WCHAR szPath[MAX_PATH];
    GetModuleFileNameW(nullptr, szPath, ARRAYSIZE(szPath));
    std::wstring exePath = szPath;
    size_t slash = exePath.find_last_of(L'\\');
    g_exeDir = (slash == std::wstring::npos) ? exePath : exePath.substr(0, slash);

    InitializeCriticalSection(&g_cs);
    ReadOriginalAnimation();
    DbgLog("orig MinAnimate=%lu enabled=%d", g_origMinAnimate, (int)IsEnabled());

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"ClassicMinMaxHostWnd";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"ClassicMinMax", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hwnd)
    {
        DbgLog("CreateWindowEx failed %lu", GetLastError());
        return 0;
    }
    DbgLog("host window created");

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    if (IsEnabled())
    {
        SetSystemMinAnimate(FALSE);
        StartMonitor();
        DbgLog("monitor started");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopMonitor();
    RestoreSystemAnimation();
    DeleteCriticalSection(&g_cs);
    if (hMutex)
        CloseHandle(hMutex);
    return 0;
}

