# Building from source

[← Back to README](../README.md)

How to build the plugin DLLs and the `.ts3_plugin` package yourself, run the unit tests and use the developer tools. Release signing and the update check are covered at the end.

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
- **Your build has no update check.** Without the author's signing key, `build.ps1` builds with `TSMEDIA_UPDATER` off, so a copy you build yourself never replaces itself with an official release and never loads `winhttp.dll`. Settings → Privacy & updates says *Updates are turned off in versions you build yourself*.
- **Cloned without `--recursive`?** Run `git submodule update --init`. The TeamSpeak plugin SDK is a git submodule.

### `build.ps1` options

| Option | Effect |
| --- | --- |
| `-Qt64 <dir>` / `-Qt32 <dir>` | Qt folders, if Qt is not in `C:\dev\Qt\5.15.2\...` |
| `-No32` | build the 64-bit DLL only |
| `-Config <Release\|Debug>` | build configuration (default `Release`) |
| `-Install` | also copy the 64-bit DLL into `%APPDATA%\TS3Client\plugins` (close TeamSpeak first) |
| `-Unsigned` | build without the update check and without signing, even when the signing key is there |
| `-SigningDir <dir>` | where the signing key and tool are (default `$env:TSMEDIA_SIGNING_DIR`, then the developer tools folder) |
| `-KeyId <n>` | the key that signs the release: 1 (daily, default) or 2 (recovery, only together with `-RevokeKeys 1`) |
| `-RevokeKeys <ids>` | key ids this release revokes for good |
| `-CaptureReference <TeamSpeak folder>` | save the exports of TeamSpeak's Qt DLLs as `reference\ts-exports\<arch>.txt` (once per TeamSpeak version), then stop |
| `-VerifyPublished` | download the published `tsmedia-update.json` of the latest release and check its signature and version (network: github.com), then stop |

Every build also checks the DLLs' imports: a DLL outside an allowlist (`winhttp.dll` must be loaded on demand only) fails the build, and with a reference list each imported Qt symbol must exist in TeamSpeak's own Qt DLLs. A plugin that doesn't load on a user's PC could never update itself again.

### Plain CMake

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\dev\Qt\5.15.2\msvc2019_64
cmake --build build --config Release
```

| CMake option | Default | Effect |
| --- | --- | --- |
| `TSMEDIA_UPDATER` | `OFF` | builds the GitHub update check (official signed releases only; the install and rollback parts are always built, so an updated DLL can settle its state) |
| `TSMEDIA_TESTHOOKS` | `OFF` | test variant, see [test hooks](#test-hooks) |
| `TSMEDIA_VERSION_OVERRIDE` | empty | test builds only: build another version number, for example `2.2.9`, to offer as an update |
| `TSMEDIA_BUILD_TESTS` | `ON` | the `tsmedia_tests` executable |
| `TSMEDIA_BUILD_TOOLS` | `ON` | the developer tools below |

## Unit tests

The 64-bit build also builds `tsmedia_tests`, one executable with a test class per area: link format and chat messages, the send pipeline against a fake TeamSpeak (`tests/fakets3.*`), file checks, albums, spoilers, presence and reactions, audio, drag-out and per-server settings, the server group, diagnostics and the updater (signed test vectors in `tests/data/update`). Run them from a *Developer PowerShell for VS 2022*:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Put Qt's `bin` folder on `PATH` to run `build\Release\tsmedia_tests.exe` directly.

## Developer tools

The 64-bit build also builds these in `build\Release`. Put Qt's `bin` folder on `PATH` to run them.

- `mfvideo_smoketest <video> <outdir>` probes and plays a video headless through Media Foundation, checks pause, seek, loop and teardown, and saves frames. It is completely silent: players stay muted at volume 0.
- `render_gallery <outdir> [<media dir>]` renders every preview, card, album, spoiler, reaction and player state, light and dark, to PNG files, and checks contrast and that nothing jumps while media loads.
- `tsmedia_update_helper` is the small restart helper of the updater (`tools/update_helper`). It is published as a signed release asset, never embedded in the DLL.

## Test hooks

`-DTSMEDIA_TESTHOOKS=ON` builds a test variant with extra logging, chat snapshots and self-test hooks that only act on localhost servers. With `-DTSMEDIA_UPDATER=ON` it also trusts the test signing key 99 and can fetch updates from a loopback address instead of GitHub ([testing without GitHub](UPDATES.md#testing-without-github)).

> [!CAUTION]
> **Never distribute a build with `TSMEDIA_TESTHOOKS` on.** `scripts\build.ps1` always builds with the hooks off.

## Official releases and signing

Only the author's builds contain the update check, because only they can be signed:

- `build.ps1` looks for the key tool `tsmedia-keys.ps1` and `release-key-<id>.dpapi` (or `.pkey`, passphrase-protected) in the signing folder. With them it turns `TSMEDIA_UPDATER` on, signs the release and writes, next to the `.ts3_plugin`, the raw update assets (`TSMedia-<version>-win64.update`, `-win32.update`, `-helper-win64.update`, `-helper-win32.update`) and `dist\tsmedia-update.json`. Without them, or with `-Unsigned`, it builds without the update check and says so.
- The manifest's *What's new* comes from the first five bullets under `### Highlights` in `docs\release-notes\v<version>.md`; the build fails without them.
- A passphrase-protected key is asked for while signing; `$env:TSMEDIA_SIGNING_PASSPHRASE` set in that PowerShell window avoids the prompt for one session. Never put it in a file or profile.
- After signing, the build checks the manifest against the public keys in `src\update\updatekeys.h` and prints the draft-first `gh release` commands.
- Private keys never belong in the repository: `*.dpapi`, `*.pkey` and `*.backup` are in `.gitignore`.

Key handling, the manifest format and the release checklist are in [updates](UPDATES.md#for-the-maintainer).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | the plugin; see the [architecture overview](ARCHITECTURE.md) for a module map |
| `src/update/` | the update check, installer and rollback |
| `src/video/`, `src/audio/` | Media Foundation playback, probing and compression; voice recording |
| `tests/` | unit tests (`tsmedia_tests`), a fake TeamSpeak and the updater's signed test vectors |
| `tools/` | developer tools (`mfvideo_smoketest`, `render_gallery`) and the update helper |
| `assets/` | the `tsmediachat` server group icon, embedded into the DLL by `cmake/embed_files.cmake` |
| `scripts/build.ps1` | builds both architectures, packages the `.ts3_plugin` and, with the key, signs the release |
| `docs/` | user guides, release notes and README images |
| `third_party/ts3client-pluginsdk` | official TeamSpeak 3 Client Plugin SDK headers (git submodule, plugin API 26) |
| `CHANGELOG.md` | version history |
