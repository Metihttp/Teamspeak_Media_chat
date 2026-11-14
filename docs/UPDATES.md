# Updates

[← Back to README](../README.md)

How TS Media chat updates itself, what it sends, how a release is checked and published, and how to
recover if an update goes wrong.

## For users

- **First install by hand.** 2.2.0 is the first version with automatic updates. Coming from 2.1 or older,
  install the latest version by hand once (double-click the `.ts3_plugin` with TeamSpeak closed); later
  versions can then arrive through the update check.
- **Off until you agree.** About 45 seconds after the first start of a version with the updater, a small
  window asks *Keep TS Media chat up to date?* Nothing is sent before you click **Turn on**. Closing the
  window asks again at the next start (twice at most). You can change it any time in
  **Settings → Privacy & updates → Updates**.
- **About once a day** (first 3 to 8 minutes after TeamSpeak starts) the plugin asks GitHub whether
  there is a new version. If there is, it shows *Update to X?* with what's new, and waits until you
  click **Update**, **Later** (asks again after 20 hours) or **Skip this version**. The exception,
  from 2.4.0 on, are [important updates](#important-updates): they install by themselves.
- **Update** downloads the new plugin files from the GitHub release, checks every file against the
  SHA-256 listed in that release, and installs them. The new version starts with the next TeamSpeak start: **Restart TeamSpeak now**
  (reopens TeamSpeak; your auto-connect bookmarks reconnect as usual) or **Restart later**.
- **Check now** in Settings, *Check for updates…* in the plugin menu, `/tsmedia update` and the
  [newer-version hint](#someone-in-this-channel-has-a-newer-version)'s link check once, also with
  automatic checks off. A check you start yourself never installs anything without a click. While a
  check or an install already runs, `/tsmedia update` says *TS Media chat is already checking for or
  installing an update.*
- Versions you build yourself never update themselves (the Updates group in Settings says so).

### Important updates

From 2.4.0 on, the author can mark a release as an important update (a fix that shouldn't wait for
everyone to click). Such a release installs without a click:

- **Who gets them:** 2.4.0 and later with automatic checks on (**Settings → Privacy & updates →
  Check for updates automatically**). Only the daily automatic check installs them; **Check now**,
  the plugin menu, `/tsmedia update` and the newer-version hint's link always show the normal
  *Update to X?* window. Older versions, and everyone with automatic checks off, get the normal offer.
- **Only with the author's signature.** The release must carry a marker file signed with the author's
  daily release key (key 1), for exactly that release: the same version and the same files, sizes and
  SHA-256 as its `tsmedia-update-v2.json` ([format](#important-marker-tsmedia-x-importantjson-payload-format-3)).
  Every downloaded byte is checked against the signed SHA-256. Without a valid marker the release is
  offered the normal way.
- **No click, no window.** It downloads and installs in the background with the same checks as
  **Update**. The chat then says *TS Media chat X is an important update and was installed
  automatically. It starts the next time you open TeamSpeak; to restart now, use Check for updates in
  the TS Media chat plugin menu.*, with a *What's new in X* link. After the next start: *Updated to TS
  Media chat X (an important update, installed automatically).*
- **TeamSpeak is never restarted for it.** The new version starts with your next TeamSpeak start; to
  start it now, **Check for updates…** in the plugin menu opens the window with **Restart TeamSpeak
  now**.
- **Skip this version doesn't hold it back**, but a version that rolled back on your PC (it didn't
  start), or one you replaced by hand with an older version (however it was installed), is never
  installed automatically again. A version you replaced by hand is still offered the normal way; one
  that rolled back is skipped, as the chat says then: only **Check now** shows it, and **Undo** in
  Settings offers it again, but never installs it without a click.
- **At most 2 silent attempts per version.** If a download or an install doesn't work out (no
  connection, a file that doesn't match, a full disk), the version is offered the normal way, without
  an error window (only an install that couldn't be undone still warns, as it does after **Update**).
  After 2 attempts it is only offered. Canceling its download (in its window, or by turning automatic
  checks off) stops silent installs for the rest of that TeamSpeak session.
- **Markers expire.** A marker counts until a date the author sets, at most 60 days after the release
  (30 by default). After that date (by your PC's clock), the release is offered the normal way.
- **Once:** with automatic checks on, 2.4.0's first start says in the chat *Important fixes for TS
  Media chat now install automatically when they are signed by its author; they start with the next
  TeamSpeak start. You can turn automatic updates off in Settings → Privacy & updates.* Until that
  line has been shown (it waits for a server connection), important updates are offered the normal
  way.

### Someone in this channel has a newer version

Since 2.2, TS Media tells the people in your channel its version (for the send window's count), unless
you turn off **Tell people in your channel that you have TS Media**; the hint below only reads what
others send. From 2.4.0 on, when someone in your channel has a newer version than any your plugin knows of, your own
chat gets one line, in that server's channel tab: *Someone in this channel has TS Media chat X (you
have Y).* with a **Check for updates** link.

- **Only you see it, and nothing is sent until you click.** The line names no one. The click is a
  manual check: one request to GitHub, also with automatic checks off, and it never installs anything
  by itself. If GitHub has a newer version, the *Update to X?* window opens; if not, the chat says
  *TS Media chat X isn't released yet. You have the latest release (Y).*, and that version isn't
  hinted again in that session.
- **Anyone can claim any version**, so the plugin is careful: only people in your channel, never
  ServerQuery clients or your own identity on another TeamSpeak; only plain versions such as `2.3.5`,
  newer than your version, the latest release and any version installed, offered or skipped; at most
  one major version ahead; each hint newer than the one before, and at most 2 hints per TeamSpeak
  session. None appear while a check or an install runs, or in versions you build yourself.
- The link is GitHub's latest-release page with a mark only this TeamSpeak session knows. The plugin
  handles a click on it in its own line; a link someone types into the chat, or one from an earlier
  session, just opens that page in your browser.

### What is sent

- One request per check: `GET https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest/download/tsmedia-update-v2.json`
  (about 2 KB; a second one for `tsmedia-update.json` only if the latest release has no such file),
  and the plugin files (a few MB) after you click Update or, for an important update with
  automatic checks on, by themselves. GitHub redirects
  downloads to `*.githubusercontent.com`; the plugin follows redirects only to GitHub hosts over HTTPS.
- From 2.4.0 on, one more small request, only when automatic checks are on, the check is the
  automatic one, and that file names a newer version for your TeamSpeak: that release's important
  marker, `GET .../releases/download/vX/TSMedia-X-important.json` (about 2 KB; most releases have
  none, and GitHub answers "not found"). Manual checks, including the newer-version hint's link, stay
  one request.
- **GitHub sees your IP address.** Windows may also check GitHub's certificate with its issuer, and a
  proxy configured in Windows sees the connection; with the
  [chat proxy](PRIVACY-SECURITY.md#chat-proxy) on (2.6), every check and download goes through it
  instead, and GitHub sees the proxy's address. Turning the chat proxy on or off doesn't interrupt an
  update download that is running: it finishes the way it started, and the next request takes the
  new route. No names, servers, chats, files or identifiers are
  sent; the User-Agent is `TSMedia-Updater/1` without a version; there are no cookies.
- Apart from [link previews](PRIVACY-SECURITY.md#link-previews) (2.4), the
  [YouTube player](PRIVACY-SECURITY.md#playing-youtube-videos) (2.5, only after you press Play) and the
  chat proxy's **Test** (2.6, one request when you press it), nothing else in the plugin uses the
  internet. The update check loads `winhttp.dll` only while a check or
  download runs.

### If something goes wrong

- Every error says what happened, and whether anything was changed. A failed download or check
  changes nothing.
- **The new version crashes while starting:** its second start puts the previous version back by
  itself and tells you in the chat; that version is then skipped, and never installed automatically
  again even if it is marked important. After **Restart TeamSpeak now**, a small helper also restores
  the previous version if TeamSpeak's log says the plugin failed to load.
- **TeamSpeak doesn't start at all** after an update: start it once with
  `"C:\Program Files\TeamSpeak 3 Client\ts3client_win64.exe" -safemode` (no plugins are loaded), or
  copy the previous DLL back by hand: `%APPDATA%\TS3Client\plugins\tsmedia\update\rollback\<old version>\tsmedia_win64.dll.bak`
  to `%APPDATA%\TS3Client\plugins\tsmedia_win64.dll` (TeamSpeak closed). Or install any release from
  GitHub by double-clicking the `.ts3_plugin` file.

## How it works

### Trust

Since 2.2.1 an update you click doesn't depend on a signing key: publishing a release on GitHub is
enough. An [important update](#important-updates) that installs without a click (2.4.0 and later)
needs both: the release on GitHub and a marker signed with the release key.

- **The source is the official repository.** The plugin reads the manifest of the latest release of
  `Metihttp/Teamspeak_Media_chat` only, over HTTPS (port 443, no user info), and follows at most 5
  redirects, each only to `github.com` or `*.githubusercontent.com`. The repository name is built into
  the DLL, and the plugin builds every download URL itself from the validated version; the manifest
  contains no addresses.
- **Every file is checked against the manifest.** Each download must have exactly the size and the
  SHA-256 the manifest lists (checked while downloading and again from the disk), its PE machine must
  match its name (win64 = x64, win32 = x86; a plugin must be a DLL, the helper an exe), and the new DLL's
  imports must all resolve in the running TeamSpeak (a DLL that wouldn't load is refused: "needs a
  manual install"). A file that doesn't match is deleted and nothing is changed.
- **Only newer versions.** Only strictly newer stable versions are offered (no downgrades, no
  pre-release suffixes). Serving an old manifest only means "no update". *Skip this version* and the
  rollback of a version that fails to start work as before.
- **Signed files are checked.** The signed `tsmedia-update.json` (format 1, what 2.2.0 reads) and the
  important marker are verified with **ECDSA P-256** through Windows CNG (`BCryptVerifySignature`,
  accepted only on `STATUS_SUCCESS`) against the public keys in `src/update/updatekeys.h` (key 1,
  key 2, and key 99 in test builds only). A signature that is present must be valid: a wrong
  signature, an unknown or revoked key, or signature fields in an unsigned manifest are refused. Only
  a signed `tsmedia-update.json` can revoke keys; a marker never does.
- **An important marker is bound to its release.** It is fetched only from the release
  `tsmedia-update-v2.json` names (`/releases/download/vX/`, never `/releases/latest/`), must be signed
  by a daily key (a marker signed with the recovery key 2 is ignored), must name the same version,
  files and package, and counts only until its `importantUntil` date, at most 60 days after its
  release date. The plugin then downloads
  against the signed SHA-256, and the version must still be newer than yours, need no manual install
  and not be on the never-auto list.
- What this means: whoever controls the GitHub repository can offer an update, so protect the account
  (see [Protect the GitHub account](#protect-the-github-account)). The same goes for a program on the
  PC that intercepts HTTPS with its own root certificate (some antivirus products and company
  proxies): it could change both the manifest and the files. For those you still have to click
  **Update**. To install something without your click, they would also need the release key 1, which
  is never on GitHub or in the repository ([the signing keys](#the-signing-keys)).

### Release assets

| Asset | Content |
| --- | --- |
| `TSMedia-X.ts3_plugin` | unchanged, for manual installs |
| `tsmedia-update-v2.json` | the manifest of this release, unsigned (format 2): what 2.2.1 and later read |
| `tsmedia-update.json` | a signed manifest (format 1), the only file 2.2.0 reads: see [the bridge](#the-bridge-for-220) |
| `TSMedia-X-win64.update`, `TSMedia-X-win32.update` | the raw plugin DLLs |
| `TSMedia-X-helper-win64.update`, `TSMedia-X-helper-win32.update` | the raw restart helper (`tools/update_helper`) |
| `TSMedia-X-important.json` | only in a release marked important (`build.ps1 -Important`): the signed marker that lets 2.3.4 and later install it without a click ([format](#important-marker-tsmedia-x-importantjson-payload-format-3)) |

Both manifests have fixed names, so `/releases/latest/download/` finds them. The marker has the
version in its name and is only fetched from its own release (`/releases/download/vX/`), so a marker
of another release can never count. The `.update` extension is opaque on purpose. No zip is parsed by
the updater. The only file the DLL carries inside it is the YouTube player helper (2.5, 64-bit, the
resource `TSMP`): it comes with every update of the DLL, and the plugin writes it to its `player` folder
and loads it only at the first Play ([architecture](ARCHITECTURE.md)).

### The bridge for 2.2.0

2.2.0 reads only `tsmedia-update.json` and accepts it only with a valid signature from a key built into
it (key 1 on the build PC, or the offline recovery key 2). So:

- **2.2.1 is signed once** (automatically by `build.ps1 -Release`, on the PC that has key 1). Its
  `tsmedia-update.json` and `tsmedia-update-v2.json` list the same files, for both architectures
  (`-No32` is refused for it: 2.2.0 users on 32-bit TeamSpeak would never be updated).
- **Every later release publishes that same signed file of 2.2.1 again** as its `tsmedia-update.json`
  (`build.ps1 -Release` downloads it from the v2.2.1 release and checks its signature). A user still on
  2.2.0 is offered 2.2.1, downloads its files from the v2.2.1 release, and after that start gets the
  newest version through `tsmedia-update-v2.json`. No key is needed for this, ever again; the v2.2.1
  release and its files must stay on GitHub.
- `build.ps1 -Release -Sign` signs `tsmedia-update.json` for a later version instead, while key 1 is
  still there: users of 2.2.0 then skip the step through 2.2.1.

### Manifest format 2 (`tsmedia-update-v2.json`)

Plain JSON, at most 48 KiB, no signature fields:

```json
{"format":2,"product":"tsmedia","version":"2.2.2","tag":"v2.2.2","published":"2026-11-02",
 "minFromVersion":"2.2.0",
 "package":{"name":"TSMedia-2.2.2.ts3_plugin","size":1203456,"sha256":"<64 lowercase hex>"},
 "files":{"plugins/tsmedia_win64.dll":{"size":1030656,"sha256":"...","machine":"x64"},
          "plugins/tsmedia_win32.dll":{"size":982528,"sha256":"...","machine":"x86"},
          "helper/tsmedia_update_helper_win64.exe":{"size":209408,"sha256":"...","machine":"x64"},
          "helper/tsmedia_update_helper_win32.exe":{"size":164352,"sha256":"...","machine":"x86"}},
 "notes":["Up to five plain-text lines from the release notes' Highlights"]}
```

- `version`: `major.minor.patch`, 1 to 3 digits each, no leading zeros, no suffix; `tag` = `"v" + version`.
- Sizes 1 byte to 32 MiB; SHA-256 as 64 lowercase hex digits; at most 32 `files` entries.
- **Unknown fields are ignored**, and unknown `files` entries too, unless they say `"required": true`
  (then: "needs a manual install"). `minFromVersion` above the installed version also means a manual
  install. Notes: at most 5, control and bidi characters removed, at most 160 characters, shown as
  plain text. `key`, `sig` or `payload` fields are refused; `revokeKeys` is never read.
- 2.2.1 and later read `tsmedia-update-v2.json` first and `tsmedia-update.json` only when the latest
  release has no `tsmedia-update-v2.json` (404). A future incompatible format gets a new asset name.

### Manifest format 1 (`tsmedia-update.json`, frozen)

```json
{"format":1,"key":1,"payload":"<base64 of the payload bytes>","sig":"<base64 of 64 bytes r||s>"}
```

Outer file at most 64 KiB; strict, canonical base64; payload at most 48 KiB. The payload has the same
fields as format 2, with `"format":1` and optionally `"revokeKeys":[1]`. The signature is ECDSA P-256
over SHA-256 of the exact payload bytes; the payload is parsed only after the signature checks out. A
manifest signed with the recovery key must revoke every daily key (`revokeKeys`); revocations are stored
for good (`state.ini [trust]`). A daily key can never revoke the recovery key or itself.

### Important marker (`TSMedia-X-important.json`, payload format 3)

Read by 2.3.4 and later. The outer file is the format 1 envelope exactly as above (at most 64 KiB,
`"key"`, `"payload"`, `"sig"`); only its payload is new, format 3:

```json
{"format":3,"product":"tsmedia","version":"2.3.5","tag":"v2.3.5","published":"2026-11-02",
 "important":true,"importantUntil":"2026-12-02",
 "package":{...},"files":{...the same entries as tsmedia-update-v2.json...},"notes":[...]}
```

- The fields are checked like format 2's. `important` must be JSON `true`; `published` and
  `importantUntil` are required `yyyy-MM-dd` dates with `published <= importantUntil <= published + 60
  days`, and the PC's UTC date must not be past `importantUntil`. `revokeKeys` is never read.
- **The payload format keeps the files apart.** No format 1 payload (the 2.2.1 bridge, or any signed
  `tsmedia-update.json`) is ever accepted as a marker, and versions before 2.3.4 refuse a format 3
  payload, so a marker can't be served as `tsmedia-update.json` either (`build.ps1` refuses that too).
  Format 3 is taken: a future format of the unsigned manifest becomes 4, with a new asset name.
- The signer must be a trusted, not revoked daily key: a marker signed with the recovery key counts
  as not important.
- It must describe the same release as `tsmedia-update-v2.json`: the version, every file (size,
  SHA-256, machine), the package, `minFromVersion` and whether a newer updater is needed;
  `published` and the notes may differ. Then the plugin installs the signed file list, not the
  unsigned one.
- A marker problem (none, a bad signature, another release, expired) is only written to the log. It
  never counts as a failed check, and the release is offered the normal way.

### On disk

`<TeamSpeak config>/plugins/tsmedia/update/`: `state.ini` (`[check]` schedule and last result,
`[install]` status `applied`/`done`/`rolledBack`/`unverified`, journal and the SHA-256 of each rollback
copy, `automatic=1` after an important update installed without a click, `[trust]` revoked keys,
`[auto]` the important updates: `never` the versions never installed without a click again (at most
16), `attemptVersion` and `attempts` the silent attempts, `announced` the one-time notice; *Skip*,
*Undo* and *Restore defaults* never touch it), `download/` (`*.part`), `staging/<ver>/*.new`,
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
  and skips that version. From 2.3.4 on it also puts that version on the never-auto list (`[auto]
  never`), whoever recorded the rollback (the boot counter, the helper or an older version), and so
  does a version replaced by hand with an older one.
- **Restart now:** the plugin writes the verified helper, starts it with only a job file, closes its
  windows and asks TeamSpeak to quit through Qt (queued, string-based, so no plugin code is on the stack
  while TeamSpeak unloads it). The helper waits for that TeamSpeak process, starts the same exe again
  with only data-free flags (`-nosingleinstance`, `-silentstart`, `-nohotkeys`, `-console`; never the
  old command line, never a `ts3server://` link), and rolls back only on positive evidence: a crash exit
  code, a new crash dump, TeamSpeak's log saying the plugin failed to load, or the DLL unloaded before
  it started. Anything else is left to the boot counter.

## For the maintainer

### Build a release

`scripts\build.ps1 -Release` builds with the update check (`TSMEDIA_UPDATER=ON`) and writes
`dist\upload-<ver>\` with exactly the files of the GitHub release: the `.ts3_plugin`, the `.update`
files and both manifests. It takes the notes from `docs\release-notes\v<ver>.md` → `### Highlights`
(fails without them, while the notes still have a `DRAFT` comment, or while `CHANGELOG.md` has no
`## [<ver>] - YYYY-MM-DD` heading), reads both manifests back and checks every listed file, and prints
the `gh release` commands. Without `-Release` it builds without the update check (a copy you build
yourself never replaces itself). It refuses DLLs with imports outside an allowlist, and checks every
imported Qt symbol against `reference\ts-exports\<arch>.txt`
(capture once per TeamSpeak version with `build.ps1 -CaptureReference "C:\Program Files\TeamSpeak 3 Client"`).

No password or key is needed, except once for 2.2.1, the bridge, and for important updates: `-Release`
signed 2.2.1's `tsmedia-update.json` with key 1 from the signing folder (`-SigningDir`, default
`$env:TSMEDIA_SIGNING_DIR` or the dev tools folder; DPAPI, no prompt). Later releases download the
signed file of v2.2.1 instead (or take `-BridgeManifest <file>`) and publish it again byte for byte:
the build refuses it unless its SHA-256 is the pinned
`6e6d802c71a89762f00b2d5da6dc50240f122e18ce25348b122ea238321a58fc`.

### Mark a release as an important update

`build.ps1 -Important [-ImportantDays <n>]` is `-Release` plus the marker
`dist\upload-<ver>\TSMedia-<ver>-important.json`: the format 3 payload (the same version, files,
package and notes as `tsmedia-update-v2.json`, `"important":true`, `importantUntil` = the release date
plus `-ImportantDays`, default 30, 1 to 60), wrapped in the format 1 envelope and signed with key 1.
Use it only for a fix that shouldn't wait: 2.3.4 and later with automatic checks on install it
without a click.

- It is always signed with key 1, never the recovery key, and refused for 2.3.4 and older (no
  version reads a marker for them), together with `-RevokeKeys`, with a `-KeyId` other than 1, and
  when key 1 or `tsmedia-keys.ps1` isn't in the signing folder (checked before the long build).
- It never touches `tsmedia-update.json`: that stays the bridge of 2.2.1, byte for byte.
- All payloads of one build carry the same `published` date. The build reads the marker back with the
  release keys (a marker signed with the test key 99 fails), checks that key 1 signed it, the format,
  `important`, the dates, that its files and package equal `tsmedia-update-v2.json`'s, and every file
  it lists against the release folder.
- The marker must be in the release before it is published (draft first, as always; with immutable
  releases it can't be added later). A marker in a release published with `--latest=false` is never
  read: clients only read the latest release's `tsmedia-update-v2.json`.
- A marker counts only for its own release. If 2.3.5 is important and 2.3.6 is not, those who missed
  2.3.5 get 2.3.6 the normal way; mark 2.3.6 as important too to keep the fix automatic.

### Protect the GitHub account

The GitHub repository is now what users trust. Recommended: two-factor authentication on the account,
a `gh` token limited to this repository, and *immutable releases* in the repository settings if GitHub
offers them (then the files of a published release can't be swapped).

- **Never rename or delete the account, and don't rename the repository.** The address
  `github.com/Metihttp/Teamspeak_Media_chat` is built into every DLL since 2.2.0. A renamed or deleted
  account frees the name `Metihttp` for someone else, and 2.2.1 and later would trust their releases;
  a renamed repository only keeps working through GitHub's redirect, which ends as soon as a new
  repository takes the old name.
- **Never delete the v2.2.1 release or its files:** they are the bridge for users of 2.2.0.

### The signing keys

- **Key 1, the daily release key, signs important markers** (and signed the 2.2.1 bridge, and
  `-Sign`). It is a DPAPI file bound to this Windows account: a new PC, a Windows reinstall or a reset
  password loses it. **Back it up** with `tsmedia-keys.ps1 Backup -KeyId 1 -OutFile <file>`
  (passphrase-encrypted) to a USB stick or a password manager. Losing it doesn't stop ordinary
  updates; it only ends important updates until a release (installed with a click) adds a new daily
  key.
- **Key 2, the recovery key, belongs offline**: back it up the same way, then remove
  `release-key-2.dpapi` from the build PC. It never marks anything important (the plugin ignores such
  a marker and `-Important` refuses it), and a `tsmedia-update.json` it signs must revoke every daily
  key, which ends important updates on every PC that reads it until a new daily key ships. 2.2.1 and
  later read revocations only from a signed `tsmedia-update.json`, and read that file only when the
  latest release has no `tsmedia-update-v2.json`: plan a revocation before it is needed.
- A new key is appended to `kReleaseKeys` in `src/update/updatekeys.h` (key 3 and so on). Never change
  or remove the bytes of a listed key: 2.2.0 and every later release trust exactly the keys they were
  built with.
- The private keys and the key tool (`tsmedia-keys.ps1`) live in the signing folder, never in the
  repository (`*.dpapi`, `*.pkey` and `*.backup` are in `.gitignore`).

### Release checklist

1. Finish `docs/release-notes/vX.md` first (delete its `DRAFT` comment) and put the release date into
   `CHANGELOG.md`: the first five bullets under `### Highlights` (plain text, at most 160 characters
   each) become the update dialog's *What's new* and are written into the manifests, so a later edit
   of the notes needs a new build.
2. `build.ps1 -Release` (`build.ps1 -Important` for an important update: decide this before the
   build, the marker can't be added to a published release). Check the import check passed for both
   architectures, and put the package's SHA-256 into the release notes' last line.
3. Install the `.ts3_plugin` with `package_inst.exe -silent` and compare the installed DLL hashes.
4. Update from the previous release on the local test server, once with *Restart now* and once with
   *Restart later*.
5. Push `main` and the tag, then `gh release create vX --verify-tag --draft ...` with every file of
   `dist\upload-X\` (the build prints the command), then `gh release edit vX --draft=false --latest`
   (backports: `--latest=false`), so "latest" never lacks a manifest. On github.com, attaching all
   files of `dist\upload-X\` to a new release and publishing it works too.
6. `build.ps1 -VerifyPublished`: downloads both manifests of the latest release and every file they
   list, and checks sizes, SHA-256 and the signature of `tsmedia-update.json`, that the signed file is
   2.2.1's or newer (2.2.1's must be the pinned bridge) and has both architectures, and that the
   manifest is the latest release's own. It also fetches that release's `TSMedia-X-important.json`:
   none means "not marked important"; one that is there fails the check when a recovery key signed it,
   its payload isn't format 3, `important` isn't `true`, its product, version or tag differ, its files
   or package differ from `tsmedia-update-v2.json`, or its dates are out of the 60-day window. It notes
   an expired marker and says who installs the release by itself.

### Testing without GitHub

- Unit tests: the `TestUpdater` class in `tsmedia_tests` (test manifests in `tests/data/update`; the signed ones made with the test key 99).
  Important updates: `importantMarker` and `importantPayloadFields` (the markers `important-valid.json`,
  `important-false.json`, `important-format1.json` and `important-mismatch.json`, read with a pinned
  date), `sameReleaseChecks`, `autoInstallRules`, `importantNames`, `neverAutoList`, `automaticNotice`
  and `stateFileAuto`; the newer-version hint: `peerHintPolicy`, `peerHintLiar` and `peerHintLink`, plus
  `TestProtocol::versionSeenSignal`, `TestChatLayout::ownPrintLinks` and `TestCoreSend::chatInfoLink`.
- A test marker: write a format 3 payload as UTF-8 without a BOM, then
  `powershell -ExecutionPolicy Bypass -File <signing folder>\make-manifest.ps1 -KeyId 99 -PayloadFile payload.json -OutFile TSMedia-X-important.json`
  (it wraps any payload in the format 1 envelope). Release builds don't trust key 99, so such a marker
  only counts in test builds and for `build.ps1 -VerifyPublished -VerifyFrom <url>`.
- Test builds (`-DTSMEDIA_TESTHOOKS=ON -DTSMEDIA_UPDATER=ON`) trust key 99 and read
  `<TeamSpeak config>/plugins/tsmedia/update_base.txt` (`http://127.0.0.1:<port>`; loopback only): all
  update requests then go to a local server with GitHub's paths. `-DTSMEDIA_VERSION_OVERRIDE=2.1.9`
  (test builds only) builds a "next version" to offer. `selftest_init_fail.txt` / `selftest_init_crash.txt`
  in the same folder make the next start fail or crash right after the boot guard (rollback tests).
  With `update_base.txt` set, *Restart now* in a test build brings TeamSpeak back with
  `ts3server://127.0.0.1?port=9987&nickname=TesterA` (the test helper accepts exactly that link), so a
  restart never connects to the tester's autoconnect bookmarks; release builds never pass a link.
  The marker comes from the same local server (`/Metihttp/Teamspeak_Media_chat/releases/download/vX/TSMedia-X-important.json`).
- In test builds, the live-test driver's `hint.inject {version, inChannel}` acts as if someone's hello
  named that version (the newer-version hint).

### Measured on TeamSpeak 3.6.2 (the S0 spike)

- Quit: triggering the main window's Quit action (the one with the shortcut Ctrl+Q) through a queued
  call quits TeamSpeak cleanly (4 of 4 runs). `qApp->quit()` crashes TeamSpeak and leaves it running
  invisibly, so the updater never uses it: without the Quit action it asks the user to restart TeamSpeak.
- TeamSpeak loads plugin DLLs only from the top of `plugins/`: nothing in `plugins/tsmedia/update/` (or its
  `rollback` folder) is loaded, whatever the extension.
- The rename trick works on NTFS: the loaded DLL can be moved to `plugins/tsmedia/update/rollback/...`
  and the staged one takes its name; TeamSpeak loads the new one on the next start.

### Still to measure on a real TeamSpeak (not possible in the automated tests)

- The rename trick on FAT32/exFAT and redirected AppData; the 32-bit client end to end; Windows Defender
  and AppLocker behaviour toward the helper; GitHub's real redirect hosts.
- An important update end to end from GitHub; the newer-version hint's link click in TeamSpeak's chat
  (Cozy, Compact and TeamSpeak classic, light and dark).
