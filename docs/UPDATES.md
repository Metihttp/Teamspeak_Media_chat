# Updates

[← Back to README](../README.md)

How TS Media chat updates itself, what it sends, how a release is signed, and how to recover if an
update goes wrong.

## For users

- **Off until you agree.** About 45 seconds after the first start of a version with the updater, a small
  window asks *Keep TS Media chat up to date?* Nothing is sent before you click **Turn on**. Closing the
  window asks again at the next start (twice at most). You can change it any time in
  **Settings → Updates**.
- **About once a day** (first 3 to 8 minutes after TeamSpeak starts) the plugin asks GitHub whether
  there is a new version. If there is, it shows *Update to X?* with what's new, and waits until you
  click **Update**, **Later** (asks again after 20 hours) or **Skip this version**.
- **Update** downloads the new plugin files from GitHub, checks them against the author's signature,
  and installs them. The new version starts with the next TeamSpeak start: **Restart TeamSpeak now**
  (reopens TeamSpeak; your auto-connect bookmarks reconnect as usual) or **Restart later**.
- **Check now** in Settings, *Check for updates…* in the plugin menu and `/tsmedia update` check once,
  also with automatic checks off.
- Versions you build yourself never update themselves (Settings → Updates says so).

### What is sent

- One request per check: `GET https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest/download/tsmedia-update.json`
  (about 2 KB), and after you click Update the plugin files (about 1 to 2 MB). GitHub redirects
  downloads to `*.githubusercontent.com`; the plugin follows redirects only to GitHub hosts over HTTPS.
- **GitHub sees your IP address.** Windows may also check GitHub's certificate with its issuer, and a
  proxy configured in Windows sees the connection. No names, servers, chats, files or identifiers are
  sent; the User-Agent is `TSMedia-Updater/1` without a version; there are no cookies.
- Nothing else in the plugin uses the internet. `winhttp.dll` is only loaded while a check or download
  runs.

### If something goes wrong

- Every error says what happened, and whether anything was changed. A failed download or check
  changes nothing.
- **The new version crashes while starting:** its second start puts the previous version back by
  itself and tells you in the chat; that version is then skipped. After **Restart TeamSpeak now**, a
  small helper also restores the previous version if TeamSpeak's log says the plugin failed to load.
- **TeamSpeak doesn't start at all** after an update: start it once with
  `"C:\Program Files\TeamSpeak 3 Client\ts3client_win64.exe" -safemode` (no plugins are loaded), or
  copy the previous DLL back by hand: `%APPDATA%\TS3Client\plugins\tsmedia\update\rollback\<old version>\tsmedia_win64.dll.bak`
  to `%APPDATA%\TS3Client\plugins\tsmedia_win64.dll` (TeamSpeak closed). Or install any release from
  GitHub by double-clicking the `.ts3_plugin` file.

## How it works

### Trust

- Every release publishes `tsmedia-update.json`: a payload signed with **ECDSA P-256** (SHA-256,
  64-byte IEEE P1363 signature). The plugin verifies it with Windows CNG (`BCryptVerifySignature`,
  accepted only on `STATUS_SUCCESS`) against public keys built into the DLL (`src/update/updatekeys.h`):
  key 1 (daily), key 2 (recovery, kept offline), and in test builds only key 99.
- The payload is parsed only after the signature checks out, and still validated field by field. The
  plugin builds every download URL itself from the validated version. Each downloaded file must match
  the signed size and SHA-256, its PE machine must match its name (win64 = x64, win32 = x86), and the
  new DLL's imports must all resolve in the running TeamSpeak (a DLL that wouldn't load is refused:
  "needs a manual install").
- Only strictly newer stable versions are offered (no downgrades, no pre-release suffixes). Serving an
  old manifest only means "no update".
- A manifest signed with the recovery key must revoke every daily key (`revokeKeys`); revocations are
  stored for good (`state.ini [trust]`). A daily key can never revoke the recovery key or itself.

### Release assets

| Asset | Content |
| --- | --- |
| `TSMedia-X.ts3_plugin` | unchanged, for manual installs |
| `tsmedia-update.json` | the signed manifest (fixed name, so `/releases/latest/download/` finds it) |
| `TSMedia-X-win64.update`, `TSMedia-X-win32.update` | the raw plugin DLLs |
| `TSMedia-X-helper-win64.update`, `TSMedia-X-helper-win32.update` | the raw restart helper (`tools/update_helper`) |

The `.update` extension is opaque on purpose. No zip is parsed by the updater, and the DLL contains no
embedded executable.

### Manifest format 1 (frozen)

```json
{"format":1,"key":1,"payload":"<base64 of the payload bytes>","sig":"<base64 of 64 bytes r||s>"}
```

Outer file at most 64 KiB; strict, canonical base64; payload at most 48 KiB. Payload:

```json
{"format":1,"product":"tsmedia","version":"2.2.0","tag":"v2.2.0","published":"2026-11-02",
 "minFromVersion":"2.1.1",
 "package":{"name":"TSMedia-2.2.0.ts3_plugin","size":1203456,"sha256":"<64 lowercase hex>"},
 "files":{"plugins/tsmedia_win64.dll":{"size":1030656,"sha256":"...","machine":"x64"},
          "plugins/tsmedia_win32.dll":{"size":982528,"sha256":"...","machine":"x86"},
          "helper/tsmedia_update_helper_win64.exe":{"size":209408,"sha256":"...","machine":"x64"},
          "helper/tsmedia_update_helper_win32.exe":{"size":164352,"sha256":"...","machine":"x86"}},
 "notes":["Up to five plain-text lines from the release notes' Highlights"],
 "revokeKeys":[1]}
```

- `version`: `major.minor.patch`, 1 to 3 digits each, no leading zeros, no suffix; `tag` = `"v" + version`.
- Sizes 1 byte to 32 MiB; SHA-256 as 64 lowercase hex digits; at most 32 `files` entries.
- **Unknown fields are ignored**, and unknown `files` entries too, unless they say `"required": true`
  (then: "needs a manual install"). `minFromVersion` above the installed version also means a manual
  install. Notes: at most 5, control and bidi characters removed, at most 160 characters, shown as
  plain text.
- A future incompatible format gets a new asset name (`tsmedia-update-v2.json`); format 1 stays.

### On disk

`<TeamSpeak config>/plugins/tsmedia/update/`: `state.ini` (`[check]` schedule and last result,
`[install]` status `applied`/`done`/`rolledBack`/`unverified`, journal and the SHA-256 of each rollback
copy, `[trust]` revoked keys), `download/` (`*.part`), `staging/<ver>/*.new`,
`rollback/<ver>/*.dll.bak`, `failed/<ver>/`, `started-<ver>`, and while restarting
`tsmedia_update_helper.exe`, `helper-job.ini`, `helper.log`. Choices live in `settings.ini`:
`updateCheck` (0 = not asked, 1 = on, 2 = off), `updateConsentAsked`, `updateSkipVersion`. *Restore
defaults* never touches them.

### Install and rollback

- The running DLL can't be overwritten, but it can be renamed on the same volume: it becomes
  `rollback/<old>/tsmedia_win64.dll.bak` and the verified new file takes its name. Every move is
  retried on sharing errors, journaled, and undone if a later step fails; "Nothing was changed" is only
  said when the plugins folder is byte-identical to before.
- **Boot counter:** the first lines of `ts3plugin_init` count starts of a just-installed version. If a
  start never finished (a crash in init), the next start restores the previous DLLs (only the two
  plugin names, each checked against its recorded SHA-256 and CPU), marks the update `rolledBack` and
  returns 1, so TeamSpeak unloads the plugin and keeps working. The previous version then warns once
  and skips that version.
- **Restart now:** the plugin writes the signed helper, starts it with only a job file, closes its
  windows and asks TeamSpeak to quit through Qt (queued, string-based, so no plugin code is on the stack
  while TeamSpeak unloads it). The helper waits for that TeamSpeak process, starts the same exe again
  with only data-free flags (`-nosingleinstance`, `-silentstart`, `-nohotkeys`, `-console`; never the
  old command line, never a `ts3server://` link), and rolls back only on positive evidence: a crash exit
  code, a new crash dump, TeamSpeak's log saying the plugin failed to load, or the DLL unloaded before
  it started. Anything else is left to the boot counter.

## For the maintainer

### Build and sign

`scripts\build.ps1` builds with the update check (`TSMEDIA_UPDATER=ON`) only when the signing key is
available (`-SigningDir`, default `$env:TSMEDIA_SIGNING_DIR` or the dev tools folder); otherwise, and
with `-Unsigned`, it builds without it. With the key it also writes the `.update` assets and
`dist\tsmedia-update.json`, takes the notes from `docs\release-notes\v<ver>.md` → `### Highlights`
(fails without them), checks the signature against `updatekeys.h`, and prints the release commands.
It refuses DLLs with imports outside an allowlist, and checks every imported Qt symbol against
`reference\ts-exports\<arch>.txt` (capture once per TeamSpeak version with
`build.ps1 -CaptureReference "C:\Program Files\TeamSpeak 3 Client"`).

### Release checklist

1. `build.ps1` (signed). Check the import check passed for both architectures.
2. Install the `.ts3_plugin` with `package_inst.exe -silent` and compare the installed DLL hashes.
3. Update from the previous release on the local test server, once with *Restart now* and once with
   *Restart later*.
4. `gh release create vX --draft ...` with every asset, then `gh release edit vX --draft=false --latest`
   (backports: `--latest=false`), so "latest" never lacks a manifest.
5. `build.ps1 -VerifyPublished`.

### Testing without GitHub

- Unit tests: `tsmedia_update_tests` (signed vectors in `tests/data/update`, made with the test key 99).
- Test builds (`-DTSMEDIA_TESTHOOKS=ON -DTSMEDIA_UPDATER=ON`) trust key 99 and read
  `<TeamSpeak config>/plugins/tsmedia/update_base.txt` (`http://127.0.0.1:<port>`; loopback only): all
  update requests then go to a local server with GitHub's paths. `-DTSMEDIA_VERSION_OVERRIDE=2.1.9`
  (test builds only) builds a "next version" to offer. `selftest_init_fail.txt` / `selftest_init_crash.txt`
  in the same folder make the next start fail or crash right after the boot guard (rollback tests).

### Still to measure on a real TeamSpeak (not possible in the automated tests)

- Which quit path exits TeamSpeak 3.6.2 cleanly (its Quit action, found by Ctrl+Q / objectName, or
  `qApp->quit()`): no crash dump, settings saved.
- That TeamSpeak doesn't load the `.dll.bak` copies in `plugins/tsmedia/update/` (they don't end in `.dll`).
- The rename trick on FAT32/exFAT and redirected AppData; the 32-bit client end to end; Windows Defender
  and AppLocker behaviour toward the helper; GitHub's real redirect hosts.
