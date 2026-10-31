# <p align="center">Left4Dead2 External Poc</p>
Left4Dead2 External Cheat is a proof-of-concept project for the Source Engine featuring an OS-ImGui overlay.<br>

This project is unfinished and can be optimized or extended with additional features. Unlike an internal cheat, it does not inject code into the game process. Instead, it creates an external overlay on top of the Left 4 Dead 2 game window.<br>

The application locates the `client.dll` and `engine.dll` module base addresses, uses known offsets (e.g. `dwLocalPlayer`) to read the game's memory, scans the entity list, and retrieves information such as entity positions, health, team, zombie class, and life state.<br>

For example, it can distinguish between Survivors, Special Infected (Smoker, Boomer, Hunter, Spitter, Jockey, and Charger), the Tank, the Witch, and Common Infected.<br>

It also reads the game's view matrix to transform 3D world coordinates into 2D screen coordinates, allowing ESP elements such as 2D boxes, snaplines, health bars, names, and distance indicators to be rendered accurately on the overlay.<br>

<p align="center">
  <img src="https://github.com/Yhzan95/Left4Dead2_External/blob/main/Screenshots/1.png?raw=true" width="49%">
  <img src="https://github.com/Yhzan95/Left4Dead2_External/blob/main/Screenshots/2.png?raw=true" width="49%">
</p>

<p align="center">
  <img src="https://github.com/Yhzan95/Left4Dead2_External/blob/main/Screenshots/3.png?raw=true" width="49%">
  <img src="https://github.com/Yhzan95/Left4Dead2_External/blob/main/Screenshots/4.png?raw=true" width="49%">
</p>

## Features
### [Esp Visuals]
- Show Allies (Toggle On/Off)
- Esp Preview Box (With real time change)
- 2D Box ESP (Outlined)
- 2D Box ESP (Corner Only)
- Healthbar ESP
- Healthbar Text
- Distance ESP
- Snapline ESP
- Show Name ESP
- Skeleton ESP (ValveBiped bone overlay, per-faction colors, adjustable thickness)
- Hide Debug Console
- Adjustable Circle Fov (With slider)
- ESP Settings Persistence (auto save/load to esp_config.ini)

### [Aimbot]
- In-process `CreateMove` hook inside `l4d2_payload.dll` (no second DLL, no IPC — target data is published render-thread → game-thread in memory)
- Aimbot Toggle / Trigger Key (side mouse 1/2, middle, right, Shift, Ctrl, Alt)
- Adjustable Smoothness and FOV
- Bone-based aiming (real hitbox-aligned bone positions from the bone matrix; falls back to height-ratio estimation when unavailable)
- Special Infected / Tank only filter
- Aim state readout (hooked / aiming / visibility blocked / standby)
- Aimbot Settings Persistence (saved to esp_config.ini, `[Aim]` section)

### [Visuals]
- Custom Crosshairs (Cross,dot,circle,Tcross)

### [Custom Colors]
#### Survivors
- Snapline Color
- ESP Name Color
- Distance Color
- Health Value Color
- 2D Box / Corner Box Color
#### Special Infected
- Snapline Color
- ESP Name Color
- Distance Color
- Health Value Color
- 2D Box / Corner Box Color
#### Tank
- Snapline Color
- ESP Name Color
- Distance Color
- Health Value Color
- 2D Box / Corner Box Color
#### Common Infected
- Snapline Color
- ESP Name Color
- Distance Color
- Health Value Color
- 2D Box / Corner Box Color
#### Other
- Crosshair Color
- Reset Colors to Default

## Install Dependencies
```bash
Visual Studio 2022
Windows SDK
OS-ImGui (included)
No additional third-party libraries are required.
```
## Build
```bash
1) Open the projects in Visual Studio 2022:
   - `Source\l4d2.vcxproj`            (injector -> l4d2.exe)
   - `Source\l4d2_payload.vcxproj`    (in-process ESP + aimbot -> l4d2_payload.dll)
2) Select the Win32 (x86) configuration (Debug or Release)
3) Build
Note: Win32 is required because Left 4 Dead 2 is a 32-bit process.
Output: Source\Debug\ or Source\Release\ contains l4d2.exe and l4d2_payload.dll side by side.
```
## Usage
```js
1) Launch Left4Dead2
2) Launch Left4dead2 external
3) Press Insert to open or close the menu
4) Enable or disable the desired features
Note: Make sure the game is running in Windowed or Borderless Windowed mode so the overlay can be displayed correctly.
```
## Disclaimer
> ⚠️ This project is intended for educational and research purposes only. Do **not** use it on VAC-secured or official servers, as doing so may result in a permanent ban.

## Acknowledgements      
- [OsImgui](https://github.com/TKazer/OS-ImGui)
- [Minhook](https://github.com/tsudakageyu/minhook)

        
