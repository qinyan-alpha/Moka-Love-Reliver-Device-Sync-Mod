# Moka Love Relive Device Sync Mod

A DLL hook mod for **Moka Love Relive** that synchronizes supported external devices with in-game motion and state.

For details, updates, and discussion, please see the EroScripts release thread:

[https://discuss.eroscripts.com/t/hypnoapp2-device-sync-mod/318924](https://discuss.eroscripts.com/t/moka-love-relive-device-sync-mod/337006/7)

## Supported Devices

- T-Code compatible devices, including **SR6**, **SSR1**, and **OSR2**
- Other compatible T-Code serial devices
- Intiface Central devices that support **LinearCmd**

## Features

- Real-time device movement synchronization with the game
- Serial T-Code output support
- Intiface Central WebSocket support
- Built-in GUI for device connection and sync settings
- Console/debug output support

## Compatibility

This mod currently only supports **2026.7.19 game update**.

Other game versions are not guaranteed to work.

## Installation

Place the provided DLL file in the game root folder.

## Default Hotkeys

- **H**: open/close the mod GUI
- **F1**: open/close the console/log window

## Source Code Notice

This game uses IL2CPP APIs and obfuscated/encrypted `global-metadata.dat` data.

To respect the author's wishes, this project will not publicly release the source code. Only this `README.md` file and the compiled DLL file are provided.
