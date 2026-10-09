# TS Media chat v2 — implementation spec

v1 (shipped): files are uploaded to the channel file browser (`/tsmedia`), a `ts3file://` BBCode link
is posted, and plugin users get inline image previews / file cards inserted into TeamSpeak's own chat
(`QTextBrowser` + `QTextDocument` image resources named `tsmedia:<key>`). Read the v1 sources before
changing them; keep their style (Qt 5.15, C++17, QStringLiteral, 4-space indent, `/W4` clean).

v2 adds: **inline video player and animated GIFs inside the chat**, a **"plugin required" note** for
people without the plugin, **metadata + BlurHash placeholders** so the chat never jumps, **previews /
posters** uploaded with big images and videos, a **gallery viewer with a video player**, a
**right-click menu** and a **cache size limit**.

The public headers in `src/` are the contract between modules. Do not change public APIs. You may add
private members/helpers to the classes you own (sections marked "implementation ... may be reorganised").

## Hard constraints

* TeamSpeak 3.6.2 ships Qt 5.15.2 Core/Gui/Widgets/Network/WebEngine… but **not Multimedia and not
  Concurrent**. Use only QtCore/QtGui/QtWidgets + Windows APIs. Video = Media Foundation (`src/video/mfvideo.*`).
  `mfplat.dll`/`mfreadwrite.dll` are delay-loaded (Windows N editions may lack them); `mf::startup()` /
  `mf::probe()` check for them first, so the plugin still loads there, just without video.
* Everything runs inside TeamSpeak's process. Never block the GUI thread for long (probing runs on
  `Core::m_pool`). No callbacks/timers/threads may outlive plugin shutdown (the DLL gets unloaded):
  objects own their timers, workers are joined, MF engines are shut down synchronously.
* Input from chat links is **untrusted** (any user can type a `ts3file://` link with any params):
  clamp `w`/`h` to 1..16384 and the aspect ratio to 1:8..8:1, `d` to >= 0, BlurHash length <= 120 and
  validated, `pv` must be an absolute path without `..` and must not equal the main file. Never decode
  images above 80 megapixels (check `QImageReader::size()` first; show a card instead). Preview files are
  downloaded automatically, so abort a preview transfer whose total size exceeds 5 MB.
  Executables are never launched (reveal in Explorer instead) — keep v1's `isRiskyToOpen` list.
* Test hooks only under `#ifdef TSMEDIA_TESTHOOKS`, only acting on localhost servers.

## Link / message protocol (`medialink.*`)

`ts3file://host?port=..&serverUID=..&channel=..&path=..&filename=..&isDir=0&size=..&fileDateTime=..`
plus TS Media params `tsm=2&w=..&h=..&d=..&bh=..&pv=..` (only the ones that apply; all values
percent-encoded with `QUrl::toPercentEncoding`). Parsing must accept plain TeamSpeak links (no `tsm`),
unknown params, any param order, `&amp;`, and `file:///ts3file/...` hrefs like v1.

`composeChatMessage(link, includeNotice, url)` = `link.toBBCode()` and, if `includeNotice`,
` [COLOR=#8e9297][I]— <note>[/I][/COLOR]` where note is (always English):
* no url: `TS Media chat plugin required to view this in chat`
* url: `[URL=<url>]TS Media chat[/URL] plugin required to view this in chat`
* Default url (Settings::defaultDownloadUrl): `https://github.com/Metihttp/Teamspeak_Media_chat`; an empty setting means the default
Keep the whole message < 1000 UTF-8 bytes (drop `bh`, then the note, if ever needed).

Receivers with the plugin hide the note: for an anchor whose href parses with `protocol >= 2`, the
text from the end of the anchor to the end of its block (excluding TS Media preview objects and the
line separators the plugin inserted) is deleted from the chat document.

## Upload pipeline (`core.cpp`, `mediaprobe.cpp`)

1. validate (connected, size limit, not empty, channel not password protected) and stage the file
   (v1 behaviour, both staging layouts);
2. `probeLocalMedia(staged file, Settings::generatePreviews)` on `m_pool`; result back on GUI thread;
3. ensure `<uploadDirectory>` and `<uploadDirectory>/previews` exist (v1 mkdir logic: wait for the
   answer or 4 s; permission error on the folder => fall back to channel root; previews folder failing
   => put the preview next to the file as `<base>.preview.jpg`);
4. if `info.preview` is set: upload it as `<previews dir>/<remoteName base>.jpg` (failure is not fatal —
   continue without `pv`);
5. upload the file (progress shown by UploadToast), then post `composeChatMessage(link with metadata,
   Settings::addRequiredNotice, Settings::pluginDownloadUrl)` to the target (channel/server/private);
6. seed the cache with the file and its preview so the sender sees it instantly (`isOwnUpload`).

## Receiving (`core.cpp`)

`ensure(link)`: register entry (sanitised metadata), cache hit => Ready. Auto downloads:
preview/poster always (if `pv` valid); images & GIFs <= `autoDownloadMaxMB` (else only preview);
videos <= `videoAutoDownloadMB` (0 = never; download starts when the user presses play).
Max 3 parallel transfers, queue the rest; previews before main files.

`still(key, maxPx)`: Full (decoded main image / first GIF frame) > Preview (poster/preview JPEG) >
BlurHash (decode at ~32 px wide with the link aspect, smooth-scale up) > None. Cached in `m_stills`
(invalidate on entry change / cache clear). Videos never have Full.

Cache: `<data>/cache/<server hash>/<channel>/<key8>_<name>`; previews likewise (their own key).
After each finished download/upload, `enforceCacheLimit()`: if the cache exceeds `cacheLimitMB`,
delete least-recently-used files (mtime, refreshed whenever a file is used) except `m_inUse` keys and
mark affected entries Idle again.

`openRequested(key)` is emitted when a download started with `openWhenReady` completes (or
immediately if Ready). ChatIntegration decides what "open" means.

## Chat UI (`chatintegration.cpp`, `inlinemedia.cpp`, `previewrenderer.cpp`)

Rendering per preview (`renderFor`):
* `media->mode(key) == Video` => `renderVideo(entry, media->frame(key), core->still(key), media->overlay(key))`
* `Animated` with a frame => `renderAnimatedFrame`
* else `renderPreview(entry, core->still(key))`
The returned logical size must equal `previewLogicalSize(entry, style)` so updates never relayout.
`onFrameChanged(key)` uses a cheap path: re-render, `addResource`, update only the preview's rect.

Look (Discord-like, dark & light): rounded 8 px corners; images/GIF/poster fill the preview rect;
BlurHash placeholder shows a subtle spinner/progress ring while the real image loads; video shows the
poster with a 56 px translucent circular play button and a duration badge (bottom-right, e.g. `0:10`);
while playing, hovering shows a bottom gradient bar: play/pause, current/total time, seek bar (thin,
accent `#5865f2`, grows on hover), mute and expand icons (draw icons with QPainterPath, no fonts/emoji).
Controls auto-hide 2.5 s after the mouse stops while playing. Ended => replay icon. Busy (downloading/
opening) => progress ring with percentage in place of the play button. Cards (other files) as in v1.

Interaction:
* image / GIF click => `openViewer(key)` (downloads first via `core->download(key, true)` if needed)
* video: `videoZoneAt` + `media->click(...)`; Expand => `openViewer`
* other files: Ready => `core->openExternally`, else `core->download(key, true)`
* right-click on a preview => menu: Open, Open with default app, Save as…, Copy image (image ready),
  Copy link (`ts3file://` url), Show in folder, Download/Retry as applicable, Play/Pause & Mute (video)
* hover over previews: pointer cursor; `media->hover(key, zone)`
* `Core::openRequested` => media kinds open the viewer; other files `openExternally`.

InlineMediaController:
* one `mf::VideoPlayer` per started video (keep at most 3, destroy least-recently used non-playing),
  only one video plays at a time; volume/mute/loop from Settings; `core->setInUse` while a player exists
  (and while a GIF animation runs; `setInUse` is counted, so the viewer's own mark is independent);
  frames are produced at the size given by `setFrameSize`; `frameChanged` on every new frame and state change.
* GIF/animated WebP: `QMovie` with `setScaledSize`, running only while visible (and hovered when
  `autoplayGifs` is off); frameChanged per frame; refuse > 80 MP total frame size.
* `setVisibleKeys` comes from ChatIntegration (every ~300 ms and on scroll): previews intersecting the
  viewport of a visible chat browser.

## Viewer (`mediaviewer.cpp`)

Port v1 ImageViewer (zoom/pan canvas, Copy / Save as / Show in folder / Open externally) and add:
gallery (Left/Right keys + buttons, "3 / 7" counter), GIF animation, and videos with a full player
(mf::VideoPlayer: play/pause button & Space, seek slider, time label, mute + volume slider, loop toggle,
F = fullscreen, Esc = close). Items not yet downloaded: call `core->download(key, false)` and show the
still with a progress indicator until Ready (listen to `Core::entryChanged`). Mark the shown key
in use. Dark theme as v1.

## Settings & user-visible texts

New settings (see `settings.h`) are persisted like v1 keys. The ranges of the numeric settings are
defined once in `settings.h` (`Settings::*Range`): `load()` clamps to them and SettingsDialog offers
exactly those ranges. SettingsDialog groups: Receiving, Playback, Sending (incl. "Tell people without
the plugin that it is needed" and the download link), General (cache limit + usage, open/clear cache).

The UI is English only (an old `language` key in `settings.ini` is ignored). Every user-visible string
(menus, toasts, dialogs, previews, the plugin-required note) still goes through `i18n::t("...")`, which
returns a Qt-allocated `QString` and not a `QStringLiteral`: such strings may end up in Qt state that
outlives the plugin DLL (style sheets, settings, TeamSpeak's widgets; see `settings.cpp`).

## Test hooks (TSMEDIA_TESTHOOKS only)

Existing: `<data>/selftest_upload.txt` (files to upload after connecting to localhost). Add:
* `<data>/selftest_play.txt` — 6 s after connecting (localhost only), press play on the most recent
  Video entry in the chat (`media()->click(key, VideoZone::Body, 0)`); consumed.
* `<data>/selftest_options.txt` containing `nohide` — do not hide the plugin-required note and do not
  insert previews (shows what users without the plugin see).
* Chat snapshots: on every preview insert/refresh/frame, at most one snapshot per 1.5 s per browser,
  numbered `debug/chat_<browser>_<NN>.png` (keep the last 30), plus a `[test]` log line.
