# SnowRunner Cruise Control HUD — memscan custom build

SnowRunner 巡航控制（Cruise Control）HUD 的本地定制分支，基于上游
**[TRSteep/SR_CruiseControl](https://github.com/TRSteep/SR_CruiseControl/)** v1.2.1 HUD 版改造。
**不修改游戏 EXE**，只重编译 mod 自身的 ASI。

## 这是什么

在加壳（SteamStub）的 Steam/repack 版 SnowRunner 上，官方 1.2.1 的 ASI 启动时直接读磁盘
EXE 做特征码扫描，读到密文导致钩子装不上、HUD 不出现。本分支把解析器改为**抓取运行时内存
镜像快照**做签名定位，在不动游戏文件的前提下让 HUD 挂载成功。

> 功能定位：**定速/自动油门保持**——巡航接通后模拟一直按住油门 `W`。它不像真车自适应巡航
> （ACC）那样用雷达测距、自动跟车减速，也不会自动刹车；没有前车距离适应。

## 功能

- 运行时内存快照签名解析：轮询等壳解密完成，不依赖磁盘 EXE 哈希白名单。
- 巡航切换：键盘 **F6**，或手柄 **十字键左键（D-pad Left）**；F6 保留可用。
- 中文界面标签显示 **“巡航”**；英文 / 俄文 / 法文原文案保留。
- 左键徽章与原生键帽（手刹 L / 全轮驱动 E / 差速锁 Q）同槽位、同缩放对齐。
- 钩子基于 [MinHook](https://github.com/TsudaKageyu/minhook)，源码随包附带
  （`ingame/vendor/minhook-1.3.4/`）。

## 目录

```
ingame/
  experimental/scaleform/   # ASI 主体：Probe.cpp / NativeUi.h / NativeControl.h 等 + 单测 + build.ps1
  compatibility/             # 签名解析器 SignatureResolver.h / SymbolPatterns.h
  src/                       # 巡航状态机 CruiseState.h
  tests/StateTests.cpp
  vendor/minhook-1.3.4/       # MinHook 源码
README_SOURCE.md             # 本次改动明细、构建与打包说明
```

## 构建

前置：WinLibs GCC 16.2.0 UCRT64（posix-seh）。在 `ingame/experimental/scaleform/` 下：

```powershell
.\build.ps1 -Toolchain <mingw64\bin 绝对路径> -Autopilot
```

产物：`ingame/build/scaleform-probe/SnowRunnerNativeAutopilot.asi`。构建会自动编译并运行
六组单测（probe / native UI / session / icon / localization / state），任一失败即中止。

## 安装

1. 把 `SnowRunnerNativeAutopilot.asi` 和 `SnowRunnerNativeAutopilot.ini` 拷到
   `SnowRunner\Sources\Bin\`。
2. 若 Bin 里已有其它模块安装的 `dinput8.dll`（ASI Loader），**保留现有加载器，不要覆盖**。
3. 进驾驶画面后，巡航班行出现在原生手刹行上方；主菜单 / 车库不可见属正常。

## 已知问题

- **与 `SnowRunner_time_control.exe` 时间模块同开会崩溃**：已二分定位到 get 观察钩子
  （RVA `0x1B5D4B0`），直通模式仍崩；在解决前不要并开。
- 接通巡航后手刹行橙色高亮：ASI 只注入油门 `W`，不发送手刹键（空格 / RB）；倾向是原生已
  拉手刹后弹出的提示高亮，待进游戏看日志按键掩码核实。
- 该游戏版本 ABI 未经上游官方验证；游戏大版本更新导致函数偏移变化时，需重新核对签名。

## 许可与归属

- 上游项目：https://github.com/TRSteep/SR_CruiseControl/
- MinHook：见 `ingame/vendor/minhook-1.3.4/LICENSE.txt`（BSD 风格）。
- 本仓库不含游戏可执行文件、加载器二进制或任何受版权保护的游戏资源。
