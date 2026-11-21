# SnowRunner Cruise Control HUD 1.2.1 — 内存扫描定制版源码包

日期：2026-11-17
上游项目：https://github.com/TRSteep/SR_CruiseControl/ （本仓库为其 1.2.1 HUD 版的
本地定制分支，改动见第三节；MinHook 许可随 `ingame/vendor/minhook-1.3.4/LICENSE.txt` 附带）。
本包为可重编译的源码快照，对应已交付 ASI `2a02594c9003999ccba451a87794604762241c972cc0802f6b24967296a51e31`（897,099 字节）。
**未改动游戏 EXE**，所有改动都在 mod 自身源码内。

## 一、目录布局（与编译脚本相对路径一致）

```
ingame/
  VERSION                                  # 1.2.1
  experimental/scaleform/                  # ASI 主体（本次改动核心）
    Probe.cpp                              # 内存快照解析 + 5 个 MinHook 钩子 + 输入/会话装配
    NativeUi.h                             # HUD 行/键帽/徽章几何与绘制（含左键徽章对齐）
    NativeControl.h                        # F6 + 手柄十字键左键切换、注入 W、掩码诊断
    NativeSession.h                        # 原生会话/行挂载/可见性镜像
    CruiseIcon.h                           # 速度表矢量图标
    HudLocalization.h                      # 英文/俄文/法文/中文“巡航”
    *Tests.cpp                             # 六组单测
    SnowRunnerNativeAutopilot.ini          # 默认配置
    build.ps1                              # 构建脚本（见下）
  compatibility/
    SignatureResolver.h                    # 内存/磁盘镜像解析与特征码定位
    SymbolPatterns.h                       # 函数签名模式
    CompatibilityReport.h / ExperimentalPolicy.h / ReviewedBuilds.h/.inc
  src/
    CruiseState.h                          # 巡航状态机（NativeControl.h 依赖）
    DrivingHud.h / HudReference.h / PanelArtwork.h / Autopilot.cpp   # 参考：旧 ImGui 独立构建
  tests/StateTests.cpp                     # 巡航状态机单测
  vendor/minhook-1.3.4/                    # MinHook 源码（hook.c/buffer.c/trampoline.c/hde64）
```

## 二、构建

前置：WinLibs GCC 16.2.0 UCRT64（posix-seh），把 `bin` 目录传给 `-Toolchain`。
在 `ingame/experimental/scaleform/` 下执行：

```powershell
.\build.ps1 -Toolchain <mingw64\bin 绝对路径> -Autopilot
```

产物：`ingame/build/scaleform-probe/SnowRunnerNativeAutopilot.asi`；构建会自动编译并运行
六组单测（probe / native UI / session / icon / localization / state），任一失败即中止。

## 三、本次相对官方 1.2.1 的改动点（仅 mod 源码）

1. **内存扫描解析**：`Probe.cpp` 启动时不再读磁盘加壳 EXE，改为 `ReadProcessMemory` 抓运行时
   镜像快照并轮询重试；`SignatureResolver.h` 新增内存镜像 `parseMemory`（RVA 映射）。
2. **手柄十字键左键**：`NativeControl.h` 动态加载 `xinput1_4/1_3.dll`，读 `DPAD_LEFT=0x0004`，
   与 F6 进入同一条切换边沿；F6 保留。
3. **中文“巡航”**：`HudLocalization.h` 新增中文枚举与 CJK 区间识别。
4. **左键徽章对齐**：`NativeUi.h` 徽章槽位 x=207,y=8、缩放 0.75，与原生 L/E/Q 键帽一致。
5. **手刹掩码诊断**：`NativeControl.h` 在切换沿记录 `lastPadMask/lt/rt`，日志标注 RB=0x0200。
6. INI 开关：`InputHook`（输入子系统总闸）、`Hooks`（MinHook 总闸）、逐钩开关、
   `HookGetPassthrough`（get 钩子直通）。

## 四、本包未包含 / 说明

- 未包含 `vendor/imgui-1.91.6` 与 `vendor/asi-loader-v10.0.2`：ASI（`-Autopilot`）构建路径
  不依赖 ImGui；加载器为二进制，按官方说明若 Bin 已有其它 ASI Loader 则保留现有那份。
- `src/Autopilot.cpp` 是旧的 ImGui 独立叠加层参考代码，本 ASI 构建不编译它；要编译它需另行
  补回 imgui vendor。
- `installer/`（C# .NET 安装器）与本源码包的 ASI 构建无关，仅随源码留档。
- 已知未解决问题：接通巡航后手刹行橙色高亮，待进游戏看日志掩码核实。详见交付 zip 内
  `README_MEMSCAN.md`。
