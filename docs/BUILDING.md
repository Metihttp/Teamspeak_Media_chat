# Building from source

[← Back to README](../README.md)

How to build the plugin DLLs and the `.ts3_plugin` package yourself, run the unit tests and use the developer tools. Official releases and the update check are covered at the end.

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
- **Your build has no update check.** Without `-Release`, `build.ps1` builds with `TSMEDIA_UPDATER` off, so a copy you build yourself never replaces itself with an official release and never loads `winhttp.dll`. Settings → Privacy & updates says *Updates are turned off in versions you build yourself*.
- **Cloned without `--recursive`?** Run `git submodule update --init`. The TeamSpeak plugin SDK is a git submodule.

### `build.ps1` options

| Option | Effect |
| --- | --- |
| `-Qt64 <dir>` / `-Qt32 <dir>` | Qt folders, if Qt is not in `C:\dev\Qt\5.15.2\...` |
| `-No32` | build the 64-bit DLL only (refused when `tsmedia-update.json` is signed, for 2.2.1 and with `-Sign`: users of 2.2.0 on 32-bit TeamSpeak need its 32-bit files) |
| `-Config <Release\|Debug>` | build configuration (default `Release`) |
| `-Install` | also copy the 64-bit DLL into `%APPDATA%\TS3Client\plugins` (close TeamSpeak first) |
| `-Release` | an official release: builds the update check in and writes `dist\upload-<version>\` with every file of the GitHub release ([official releases](#official-releases)) |
| `-Sign` | also sign `tsmedia-update.json` for this version with key 1, so users of 2.2.0 update to it directly (needs the key; 2.2.1 is signed automatically) |
| `-SigningDir <dir>` | where the signing key and tool are (default `$env:TSMEDIA_SIGNING_DIR`, then the developer tools folder) |
| `-KeyId <n>` / `-RevokeKeys <ids>` | the key that signs (1, default) and key ids a signed release revokes for good |
| `-BridgeManifest <file>` | the signed `tsmedia-update.json` of v2.2.1 to reuse, instead of downloading it from GitHub |
| `-CaptureReference <TeamSpeak folder>` | save the exports of TeamSpeak's Qt DLLs as `reference\ts-exports\<arch>.txt` (once per TeamSpeak version), then stop |
| `-VerifyPublished` | download both manifests of the latest release and every file they list, check sizes, SHA-256 and the signature of `tsmedia-update.json`, that the signed file still serves 2.2.0 users on both architectures, and that the manifest belongs to the latest release (network: github.com), then stop. `-VerifyFrom <url>` checks a local test server with GitHub's paths instead |

Every build also checks the DLLs' imports: a DLL outside an allowlist (`winhttp.dll` must be loaded on demand only) fails the build, and with a reference list each imported Qt symbol must exist in TeamSpeak's own Qt DLLs. A plugin that doesn't load on a user's PC could never update itself again.

### Plain CMake

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\dev\Qt\5.15.2\msvc2019_64
cmake --build build --config Release
```

| CMake option | Default | Effect |
| --- | --- | --- |
| `TSMEDIA_UPDATER` | `OFF` | builds the GitHub update check (official releases, `build.ps1 -Release`; the install and rollback parts are always built, so an updated DLL can settle its state) |
| `TSMEDIA_TESTHOOKS` | `OFF` | test variant, see [test hooks](#test-hooks) |
| `TSMEDIA_VERSION_OVERRIDE` | empty | test builds only: build another version number, for example `2.2.9`, to offer as an update |
| `TSMEDIA_BUILD_TESTS` | `ON` | the `tsmedia_tests` executable |
| `TSMEDIA_BUILD_TOOLS` | `ON` | the developer tools below |

## Unit tests

The 64-bit build also builds `tsmedia_tests`, one executable with a test class per area: link format and chat messages, the send pipeline against a fake TeamSpeak (`tests/fakets3.*`), file checks, albums, spoilers, presence and reactions, replies, emoji (the table, the renderer, HD emoji in chat documents, together with reply lines), audio, drag-out and per-server settings, the server group, diagnostics and the updater (test manifests in `tests/data/update`). Run them from a *Developer PowerShell for VS 2022*:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Put Qt's `bin` folder on `PATH` to run `build\Release\tsmedia_tests.exe` directly.

## Developer tools

The 64-bit build also builds these in `build\Release`. Put Qt's `bin` folder on `PATH` to run them.

- `mfvideo_smoketest <video> <outdir>` probes and plays a video headless through Media Foundation, checks pause, seek, loop and teardown, and saves frames. It is completely silent: players stay muted at volume 0.
- `render_gallery <outdir> [<media dir>]` renders every preview, card, album, spoiler, reaction and player state, light and dark, to PNG files, and checks contrast and that nothing jumps while media loads.
- `tsmedia_update_helper` is the small restart helper of the updater (`tools/update_helper`). It is published as a release asset, never embedded in the DLL.

## Test hooks

`-DTSMEDIA_TESTHOOKS=ON` builds a test variant with extra logging, chat snapshots and self-test hooks that only act on localhost servers. With `-DTSMEDIA_UPDATER=ON` it also trusts the test signing key 99 and can fetch updates from a loopback address instead of GitHub ([testing without GitHub](UPDATES.md#testing-without-github)).

> [!CAUTION]
> **Never distribute a build with `TSMEDIA_TESTHOOKS` on.** `scripts\build.ps1` always builds with the hooks off.

## Official releases

`build.ps1 -Release` makes an official release. No password is needed, and no key either, except once for 2.2.1 (key 1 on the build PC, read without a prompt):

- It turns `TSMEDIA_UPDATER` on and writes `dist\upload-<version>\` with exactly the files of the GitHub release: `TSMedia-<version>.ts3_plugin`, the raw update files (`TSMedia-<version>-win64.update`, `-win32.update`, `-helper-win64.update`, `-helper-win32.update`), `tsmedia-update-v2.json` (unsigned; lists the size and SHA-256 of every file) and `tsmedia-update.json` (signed, for users of 2.2.0).
- `tsmedia-update.json`: for 2.2.1, the bridge release, the build signs it with key 1 from the signing folder (DPAPI, no prompt) and stops if the key isn't there. Every later release reuses the published, signed file of v2.2.1 (downloaded and checked by the build), so users of 2.2.0 update to 2.2.1 first and from there to the newest version. Details: [the bridge](UPDATES.md#the-bridge-for-220).
- The manifests' *What's new* comes from the first five bullets under `### Highlights` in `docs\release-notes\v<version>.md`; the build fails without them, while the notes still contain a `DRAFT` comment, or while `CHANGELOG.md` has no `## [<version>] - YYYY-MM-DD` heading for the release.
- The build reads both manifests back, checks every file they list, and prints the draft-first `gh release` command. Uploading all files of `dist\upload-<version>\` through GitHub's web page (*Draft a new release*, then drop the files in) works too.
- After publishing, `build.ps1 -VerifyPublished` downloads the latest release's manifests and files and checks them.
- Since the GitHub repository is what users trust, protect the account: two-factor authentication is recommended. Never rename or delete the account (someone else could take its name) or rename the repository, and never delete the v2.2.1 release ([details](UPDATES.md#protect-the-github-account)).
- Private keys never belong in the repository: `*.dpapi`, `*.pkey` and `*.backup` are in `.gitignore`.

The manifest formats and the release checklist are in [updates](UPDATES.md#for-the-maintainer).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | the plugin; see the [architecture overview](ARCHITECTURE.md) for a module map |
| `src/update/` | the update check, installer and rollback |
| `src/video/`, `src/audio/` | Media Foundation playback, probing and compression; voice recording |
| `tests/` | unit tests (`tsmedia_tests`), a fake TeamSpeak and the updater's test manifests |
| `tools/` | developer tools (`mfvideo_smoketest`, `render_gallery`) and the update helper |
| `assets/` | the `tsmediachat` server group icon, embedded into the DLL by `cmake/embed_files.cmake` |
| `scripts/build.ps1` | builds both architectures, packages the `.ts3_plugin` and, with `-Release`, writes the release folder |
| `docs/` | user guides, release notes and README images |
| `third_party/ts3client-pluginsdk` | official TeamSpeak 3 Client Plugin SDK headers (git submodule, plugin API 26) |
| `CHANGELOG.md` | version history |
