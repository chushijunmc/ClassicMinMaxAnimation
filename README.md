# ClassicMinMax — 经典最小化/最大化动画（独立版）

把 Windhawk 插件 *Classic Minimize/Maximize Animations*（作者 [aubymori](https://github.com/aubymori)，MIT 许可）的经典动画行为，做成一个**不依赖 Windhawk** 的独立程序：托盘宿主 + 进程注入器 + 钩子 DLL。

---

## 一、项目概述

Windows 95–XP 的最小化/最大化动画是「标题栏沿屏幕飞向任务栏」；Vista 之后 DWM 接管动画，变成缩放淡出。本项目的目标是在 Win7 / Win8.1 / Win10 / Win11 上恢复前者的观感：

- 最小化：窗口标题栏飞向任务栏按钮位置
- 最大化：标题栏飞向最大化后的标题栏位置
- 还原：从任务栏/最大化状态飞回原位置
- 同时支持 MDI 子窗口（在父窗口客户区内裁剪）

程序形态：

| 组成 | 说明 |
|---|---|
| `ClassicMinMax.exe` | 托盘宿主：菜单开关、开机自启、退出；同时是注入器 |
| `MinMaxHook64.dll` / `MinMaxHook32.dll` | 注入到目标进程的钩子 DLL，动画实际在目标进程内绘制 |
| `out\` 目录 | 三个文件必须放在**同一目录**，缺一不可 |

效果与 Windhawk 版一致；默认仅开启核心动画，副作用较大的兼容性补丁默认关闭。

---

## 二、实现原理

### 1. 整体架构

```
ClassicMinMax.exe (管理员, 托盘)
 ├─ 托盘窗口 (HWND_MESSAGE) + Shell_NotifyIcon
 ├─ MonitorThread：每 ~1s 扫描进程 → 注入 DLL
 ├─ 设置：HKCU\Software\ClassicMinMax
 └─ schtasks 计划任务（开机自启）

目标进程 (explorer.exe / notepad.exe / ...)
 └─ MinMaxHook64/32.dll  (MinHook inline hook user32 导出)
      ├─ 捕获 WM_SYSCOMMAND / ShowWindow
      ├─ 独立动画线程 + 分层置顶窗口
      └─ 250ms 逐帧绘制「经典标题栏」
```

### 2. 进程注入

1. `MonitorThread` 每约 1 秒用 `CreateToolhelp32Snapshot` 枚举进程，只处理**当前会话**，并跳过系统关键进程黑名单（`csrss/winlogon/lsass/dwm/svchost` 等）、AppContainer 进程和自身。
2. 用 `IsWow64Process` 判定目标位数，选择 `MinMaxHook32.dll` 或 `MinMaxHook64.dll`。
3. 跨位数注入：`OpenProcess` → 解析远端 `kernel32!LoadLibraryW` 地址。由于导出转发（kernel32 → KERNELBASE），实现了**导出转发器递归解析**（最多 4 层）。
4. `CreateRemoteThread` 调用 `LoadLibraryW`；判定成功用**远端线程退出码非 0**（64 位宿主枚举不到 WOW64 的 32 位模块，所以不能靠模块列表判断）。
5. 每个 PID 只尝试一次（内存 `std::set` + 每 10 秒清理已退出 PID），避免对沙盒进程反复重试。

### 3. 钩子点位（全部是 user32 导出）

| API | 作用 |
|---|---|
| `DefWindowProcW/A`、`DefFrameProcW/A`、`DefMDIChildProcW/A`、`DefDlgProcW/A` | 捕获 `WM_SYSCOMMAND` 中的 `SC_MINIMIZE / SC_MAXIMIZE / SC_RESTORE` |
| `ShowWindow`、`ShowWindowAsync` | 捕获 `SW_MINIMIZE / SW_MAXIMIZE / SW_RESTORE`，把 `SW_*` 映射为 `SC_*` |
| `SystemParametersInfoW/A` | `SPI_GETANIMATION` 谎报「动画开启」；吞掉 `SPI_SETANIMATION` |
| `SetWindowPos`（仅 explorer，可选） | topmost bug 缓解：调用前 `Sleep(1)`，**默认关闭** |

触发条件：窗口有 `WS_CAPTION`、当前可见、全局未在动画中（`g_fAnimating` 互斥）、开关已启用。

### 4. 动画绘制

- 独立线程创建动画窗口：`WS_POPUP + WS_EX_TOPMOST/TOOLWINDOW/NOACTIVATE/LAYERED/TRANSPARENT`，类名 `ClassicMinMax_AnimWindow`。
- `BitBlt` 抓取**整个虚拟桌面**作为底图；按「当前窗口 → 目标位置（任务栏按钮 / 最大化矩形 / 原位置）」线性插值。
- 每帧在底图上重绘被移动的标题栏（图标、渐变标题、标题文字、RTL 适配），只 `BitBlt` 脏区到动画窗口；MDI 子窗口额外用裁剪区域避开上层窗口。
- 时长固定 250ms，**无 Sleep 忙循环**（与原插件一致），结束后动画窗 `HWND_BOTTOM` 并隐藏。
- 真正的 `ShowWindow_orig` 在动画**之后**调用，因此调用线程会被同步阻塞约 250ms —— 与 Windhawk 版行为相同。

### 5. 与系统动画的协同

宿主启动时读取 `SPI_GETANIMATION`，把原值写入 `HKCU\Software\ClassicMinMax\OrigMinAnimate`，再 `SPI_SETANIMATION` 关闭 DWM 原生动画；DLL 则对应用谎报「系统动画仍开启」，避免应用因设置被关闭而走无动画分支。退出时按原值还原。

### 6. Win7 / Win8.1 兼容

Win10 才有的 DPI 接口全部改为运行时 `GetProcAddress` + 降级实现，`shcore.dll` 改为动态 `LoadLibrary`：

| 接口 | 降级 |
|---|---|
| `SetProcessDpiAwarenessContext`（宿主） | 回退 `SetProcessDPIAware` |
| `GetDpiForWindow` | 屏幕 DC 的 `LOGPIXELSX` |
| `GetSystemMetricsForDpi` | `MulDiv(GetSystemMetrics, dpi, 96)` |
| `SystemParametersInfoForDpi` | `SystemParametersInfoW`（96 DPI） |
| `GetDpiForMonitor`（shcore） | 无则按窗口 DPI 原样换算，避免二次缩放 |
| `GetWindowDpiAwarenessContext` / `AreDpiAwarenessContextsEqual` | 视为非 Per-Monitor |

最终二进制只静态导入 `USER32 / GDI32 / SHELL32 / ADVAPI32 / KERNEL32`，`/MT` 静态 CRT，Win7 SP1 可直接运行（无需 UCRT 补丁）。

### 7. 权限与自启

- 非管理员启动时 `ShellExecuteExW`（`runas`）自提权，退出原实例。
- 单实例互斥体 `Global\ClassicMinMaxHost`。
- 自启：`schtasks /Create /TN ClassicMinMax /TR "<exe>" /SC ONLOGON /RL HIGHEST /F`；关闭时 `/Delete`。

---

## 三、项目结构

```
ClassicMinMax/
├─ build.bat                     # 构建脚本（MSVC 14.44，输出到 out\）
├─ icon.png                      # 图标源图（2048×2048）
├─ README.md
├─ res/
│  └─ app.ico                    # 多尺寸图标 16/20/24/32/40/48/64/96/128/256
├─ src/
│  ├─ host.cpp                   # 托盘宿主：注入器 / 菜单 / 自启 / 设置 / 崩溃保护
│  ├─ minmax_dll.cpp             # 注入的钩子 DLL：钩子安装 + 动画实现
│  └─ app.rc                     # 资源脚本（图标 ID 101）
├─ third_party/
│  └─ minhook/
│     ├─ MinHook.h
│     ├─ libMinHook-x64-v141-mt.lib
│     └─ libMinHook-x86-v141-mt.lib
└─ out/                          # 构建产物（交付三件套）
   ├─ ClassicMinMax.exe
   ├─ MinMaxHook64.dll
   └─ MinMaxHook32.dll
```

### 运行时数据位置

| 内容 | 位置 |
|---|---|
| 设置 | `HKCU\Software\ClassicMinMax` |
| 计划任务 | 任务计划程序中的 `ClassicMinMax` |
| 调试日志（`Debug=1` 时） | `C:\Users\Public\ClassicMinMax.log` |

设置项：

| 名称 | 默认 | 含义 |
|---|---|---|
| `Enabled` | `1` | 动画总开关（DLL 侧 1 秒缓存，托盘切换后 ≤1s 生效） |
| `TopmostFix` | `0` | explorer 专用 `SetWindowPos + Sleep(1)` 补丁；改后需重启资源管理器 |
| `OrigMinAnimate` | 系统原值 | 退出时还原系统动画设置 |
| `Debug` | `0` | 写入详细 TRACE 日志 |

---

## 四、构建与使用

```bat
:: 需要 VS 2022 Build Tools（默认路径 C:\BuildTools）
build.bat
:: 成功时最后一行输出 BUILD_OK
```

使用：

1. 双击 `out\ClassicMinMax.exe`，同意 UAC；托盘出现图标即启动成功。
2. 右键托盘：启用/禁用动画、开机自动启动、退出。
3. 移动了程序目录后，需要重新勾选一次「开机自动启动」（计划任务里记录的是绝对路径）。
4. 排查问题：把 `HKCU\Software\ClassicMinMax` 的 `Debug` 设为 `1`，日志会记录每次动画的 `anim begin/end` 与耗时。

---

## 五、综述与设计取舍

- **为什么是注入而不是全局钩子/替换系统文件**：Windhawk 的原始实现依赖「每进程注入 + 用户态 inline hook」，这是唯一能同时兼容任意第三方程序、又不需要驱动与重启的路线；本项目沿用同一路线，只是把 Windhawk 的运行时换成自带的 MinHook。
- **为什么宿主和 DLL 分离**：动画必须在**目标进程**内绘制（才能拿到该进程的窗口、图标、DPI 上下文）；托盘与扫描逻辑放在一个常驻的 64 位宿主里，避免每个进程重复做进程枚举。
- **为什么动画用忙循环**：与原插件保持一致，250ms 固定时长、逐帧插值，帧率由绘制耗时决定；加入 Sleep 反而会让动画时长不可控。
- **为什么默认关闭 explorer 的 topmost 补丁**：该补丁给资源管理器内**每一次** `SetWindowPos` 加 1ms 延迟，在还原窗口这类密集调用场景会累积成可感知的卡顿；而它的收益只是缓解偶发的「窗口莫名置顶」。因此对齐 Windhawk 的默认值（关闭），需要时手动开。
- **已知限制**：
  - 动画是同步的，调用方线程会阻塞约 250ms（点击后动画播完窗口才真正最小化/还原）。
  - Win7/8.1 没有按窗口 DPI 的接口，125% 及以上缩放下标题栏图标/文字可能有 1–2px 偏差。
  - AppContainer（UWP/沙盒）进程、受保护进程不注入。
  - 32 位目标进程由 32 位 DLL 处理，注入成功以远端线程返回码判定（64 位宿主无法枚举 WOW64 模块）。

---

## 六、制作工具

| 用途 | 工具 |
|---|---|
| 代码编写、重构、文档生成 | **DeepSeek V4.1 Flash API** |
| 编译器 | MSVC 14.44.35207（VS 2022 Build Tools，`/MT /O2 /utf-8`） |
| Hook 库 | MinHook 1.3.3（x64 / x86 预编译静态库） |
| 原始插件参考 | aubymori — *Classic Minimize/Maximize Animations* 1.1.0（Windhawk，MIT） |

> 动画算法与经典行为来自上述 Windhawk 插件（MIT 许可）；本项目为其独立重实现，额外增加了托盘、自启、注入器、Win7/8.1 降级与设置持久化。
