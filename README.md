# Moka Love Relive Device Sync Mod

Windows x64 native `version.dll` proxy that synchronizes T-Code devices
(SR6, SSR1, OSR2) and compatible Intiface Central devices with **Moka Love Relive**.

面向 **Moka Love Relive** 的设备同步模组，使用 Live2D 实际动画播放时间驱动设备。
支持串口 T-Code、Intiface Central、中英文设置窗口和高潮/余韵模式。

[Discussion / 发布讨论](https://discuss.eroscripts.com/t/moka-love-relive-device-sync-mod/337006)

## Compatibility / 兼容版本

Targets game **2.0.3 / the 2026-07-19 update**, Unity **2022.3.62f2**,
Windows **x64**. Other versions require revalidation. This is the Moka project;
it does not include the HypnoApp or Shimmering Horizon mods.

## Motion synchronization / 同步原理

The hooks read the active MotionState in both Live2D controllers and evaluate
motion curves using Unity's `AnimationClipPlayable` time, following playback
speed, pauses and loops. Episode 1 uses `ParamPistonMain`; Episodes 2/3 use
`ParamBodyAngleUD3`, falling back to `ParamBodyAngleY` for some clips.

| T-Code channel | Input |
|---|---|
| L0 stroke | Main insertion-depth curve |
| L1 surge | Episode 2/3 `ParamBodyAngleUD` |
| L2 sway | Centered |
| R0 twist | Centered |
| R1 roll | Episode 1 `ParamPinstonAngle`; Episode 2/3 `ParamBodyAngleZ` |
| R2 pitch | Episode 2/3 `ParamBodyAngleY`, unless used as the main depth fallback |

Missing auxiliary inputs return to center. Disabling **Enable SR6 6-axis** sends
only L0. Active L0 oscillation suppression removes small intermediate reversals
and uses monotone PCHIP interpolation; it can be disabled in the GUI.
The output interval is configurable from 8–500 ms (default 20 ms).

### Locally generated curves / 本地生成曲线

`tools/generate_motion_curves.py` extracts the required parameters from your own
installation into `src/generated_motion_curves.h`. The header is ignored by Git
because it contains game-derived animation data. The supported build produces
42 motion groups and 105 parameter curves. Python/UnityPy is required for this
step only, not to run the compiled DLL. Asset IDs and hook addresses are version-specific.

曲线头文件不随源码发布，首次编译需从自己的游戏安装目录生成；游戏更新后需重新核对
资源 ID、函数地址和调用约定，不能只重新生成曲线就假定兼容。

| Hook | RVA |
|---|---|
| Episode 1 MotionState update | `0x421020` |
| Episode 2/3 MotionState update | `0x459B50` |
| Playable time | `0x1C8AFC0` |
| Playable validity | `0x1C8B120` |

## Build prerequisites / 编译环境

- Windows x64; Visual Studio 2022 with **Desktop development with C++** and a Windows SDK.
- CMake 3.20 or newer, available on `PATH`.
- MinHook is included under `external/minhook`; no vcpkg or BepInEx is required.

Python 3.10+ is also required for local curve generation:

```powershell
python -m pip install -r .\tools\requirements.txt
.\build.ps1 -GameRoot "D:\path\to\MokaLoveRelive"
```

Replace the example with your own game directory containing `MocaLoveRelive_Data`.
Subsequent builds can use `.\build.ps1` without `-GameRoot` while the generated
header is present. To regenerate explicitly:

```powershell
python .\tools\generate_motion_curves.py --game-dir "D:\path\to\MokaLoveRelive" --output .\src\generated_motion_curves.h
```

Output: `build/Release/version.dll`. The scripts work when invoked from another
working directory and stop on configuration/build failure. `build.bat` forwards
its arguments to `build.ps1`. Only x64 is supported.

## Installation / 安装

1. Close the game. Build the DLL or obtain a binary from this repository's
   [Releases](https://github.com/qinyan-alpha/Moka-Love-Reliver-Device-Sync-Mod/releases), checking its game-version compatibility.
2. Back up any existing `version.dll` in the game directory. Copy
   `build/Release/version.dll` alongside `MocaLoveRelive.exe`.
3. Start the game. Press **H** for the settings window; **F1** toggles the console.
4. Select the serial COM port (default baud rate: **115200**) or Intiface Central
   (`ws://localhost:12345` by default), then connect from the GUI. Automatic
   connection is disabled by default. Start with a small travel range and test
   the direction before enabling normal synchronization.

The game directory receives `MokaLoveReliveMotionSync.ini` and `MokaLoveReliveMotionSync.log`.
To uninstall, close the game and remove this mod's `version.dll`, or restore
its previous backup. Configuration and logs may also be removed.
Two DLL proxies named `version.dll` cannot simply be installed side by side.

关闭游戏后，把编译得到的 `version.dll` 放到游戏 EXE 同目录；已有同名文件请先备份。
进入游戏后按 **H** 设置设备，**F1** 查看日志。默认不自动连接设备。
卸载时删除本模组 DLL 或恢复原备份即可。

## Source layout / 源码结构

- `src/main.cpp`: game-specific hooks, address validation and input collection.
- `src/motion_input.*`: shared motion state.
- `src/sr6_sync.*`: serial T-Code, Intiface, settings UI and hotkeys.
- `src/orgasm_state.*`: climax/afterglow state.
- `src/version_proxy.*`: Windows `version.dll` export forwarding.
- `external/minhook/`: vendored hooking library and original license.

## Open source / 开源说明

The mod source is now published under the [MIT license](LICENSE), replacing the
previous README statement that only binaries would be available. Third-party
code retains its own license; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
This repository contains the mod implementation, not the original game's code,
binaries, metadata dumps, saves or assets. You need your own game installation.
This is an unofficial community mod and is not affiliated with the game developer.

本仓库现已公开模组源码，采用 MIT 许可证；原 README 中“不公开源码”的说明已撤下。
第三方依赖遵循各自许可证，原游戏内容不属于本仓库的 MIT 授权范围。

## Troubleshooting / 排错

- No UI: verify the game version, x64 build, DLL location and mod log.
- `target validation failed` or a signature mismatch: the executable does not
  match the expected hook targets. Recheck the game's RVAs and ABI before porting;
  passing a few prefix checks alone does not prove compatibility with a new version.
- No device movement: check the COM port or Intiface server/device selection,
  connection status, and whether a supported interaction is active.
- Report the game version, build environment, reproduction steps and relevant
  log excerpt in [Issues](https://github.com/qinyan-alpha/Moka-Love-Reliver-Device-Sync-Mod/issues). Remove personal paths from shared logs.
