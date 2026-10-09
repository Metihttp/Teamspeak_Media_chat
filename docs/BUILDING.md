# Building from source

[← Back to README](../README.md)

How to build the plugin DLLs and the `.ts3_plugin` package yourself, run the unit tests and use the developer tools.

## Requirements

- Windows 10 or 11
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with the **Desktop development with C++** workload (MSVC, the Windows SDK and the bundled CMake)
- Qt 5.15.2 for MSVC 2019, both 64-bit (`msvc2019_64`) and 32-bit (`msvc2019`). This is the Qt version TeamSpeak 3.6 ships. The easiest way to get it is [aqtinstall](https://github.com/miurahr/aqtinstall).
- Git and Python 3

## Build

```powershell
git clone --recursive https://github.com/Metihttp/Teamspeak_Media_chat.git
cd Teamspeak_Media_chat

python -m pip install aqtinstall
python -m aqt install-qt windows desktop 5.15.2 win64_msvc2019_64 --archives qtbase -O C:\dev\Qt
python -m aqt install-qt windows desktop 5.15.2 win32_msvc2019 --archives qtbase -O C:\dev\Qt

powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

- **Output:** `dist\TSMedia-<version>.ts3_plugin`, containing `tsmedia_win64.dll` and `tsmedia_win32.dll` in the same layout as packages from myteamspeak.com. The version comes from `project(... VERSION ...)` in `CMakeLists.txt`.
- **Cloned without `--recursive`?** Run `git submodule update --init`. The TeamSpeak plugin SDK is a git submodule.

### `build.ps1` options

| Option | Effect |
| --- | --- |
| `-Qt64 <dir>` / `-Qt32 <dir>` | Qt folders, if Qt is not in `C:\dev\Qt\5.15.2\...` |
| `-No32` | build the 64-bit DLL only |
| `-Config <Release\|Debug>` | build configuration (default `Release`) |
| `-Install` | also copy the 64-bit DLL into `%APPDATA%\TS3Client\plugins` (close TeamSpeak first) |

### Plain CMake

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\dev\Qt\5.15.2\msvc2019_64
cmake --build build --config Release
```

## Unit tests

The 64-bit build also builds `tsmedia_tests` (link format, chat message, BlurHash and helpers). Run them from a *Developer PowerShell for VS 2022*:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Developer tools

The 64-bit build also builds two tools in `build\Release`. Put Qt's `bin` folder on `PATH` to run them.

- `mfvideo_smoketest <video> <outdir>` probes and plays a video headless through Media Foundation, checks pause, seek, loop and teardown, and saves frames. It is completely silent: players stay muted at volume 0.
- `render_gallery <outdir> [<media dir>]` renders every preview, card and player state, light and dark, to PNG files.

## Test hooks

`-DTSMEDIA_TESTHOOKS=ON` builds a test variant with extra logging, chat snapshots and self-test hooks that only act on localhost servers.

> [!CAUTION]
> **Never distribute a build with `TSMEDIA_TESTHOOKS` on.** `scripts\build.ps1` always builds with the hooks off.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | the plugin; see the [architecture overview](ARCHITECTURE.md) for a module map |
| `tests/` | unit tests (`tsmedia_tests`) |
| `tools/` | developer tools (`mfvideo_smoketest`, `render_gallery`) |
| `scripts/build.ps1` | builds both architectures and packages the `.ts3_plugin` |
| `docs/` | user guides, release notes and README images |
| `third_party/ts3client-pluginsdk` | official TeamSpeak 3 Client Plugin SDK headers (git submodule, plugin API 26) |
| `CHANGELOG.md` | version history |
